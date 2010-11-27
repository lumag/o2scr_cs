#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "o2scr.h"

static struct class *o2scr_class;
static dev_t o2scr_devt;
#define O2SCR_DEV_MAX 4

/* MMIO registers */
#define MANUAL_E_R      0x00
#define FRQ_MODE        0x02
#define MODE            0x04
#define CARD_MODE       0x06
#define PROTO           0x08
#define ETU_INI         0x0A
#define ETU_WRK         0x0C
#define CGT             0X0E
#define CWT             0x10 /* l */
#define BWT             0x14 /* l */
#define CLK_CNT         0x18
#define ETU_CNT         0x1A
#define MASK_IT         0x1C
#define FIFO_LEV        0x1E
#define EXE             0x20
#define STATUS_IT       0x22
#define DEVAL_IT        0x24
#define STATUS_EXCH     0x26
#define FIFO_NB         0x28

/*O2 Internal Register*/
#define O2_POWER_DELAY_REG 0xC4

/* IO registers */
#define MANUAL_IN	0x00
#define MANUAL_OUT	0x02
#define FIFO_IN		0x04
#define FIFO_OUT	0x06
#define XOR_REG		0x08
#define CRC16_MSB	0x0a
#define CRC16_LSB	0x0c
#define MOTO_CFG	0x10


/* MODE */
#define MANUAL      0x08
#define ATR_TO      0x10
#define EDC         0x40
#define CRD_DET     0x80

/* EXE */
#define POF_EXE     0x8000
#define PON_EXE     0x4000
#define RST_EXE     0x2000
#define EXCH_EXE    0x1000
#define CHG_ETU_EXE 0x0800
#define PTS_EXE     0x0400
#define S_TOC_EXE   0x0200
#define RESET_EXE   0x0100
#define CLK_SLEEP_EXE   0x0080
#define CLK_WAKE_EXE    0x0040
#define RST_FIFO_EXE    0x0020

/* MASK_IT, STATUS_IT, DEVAL_IT */
#define SCP             0x80
#define SCI             0x40
#define CLK_IT		0x20
#define ETU_IT		0x10
#define IT_REC          0x02
#define END_EXE         0x01

/* STATUS_EXCH  - see in o2scr.h */

static DEFINE_IDR(o2scr_idr);
static DEFINE_MUTEX(idr_lock);

static inline u8 o2scr_inb(struct o2scr_info *info, int reg)
{
	return ioread16(info->io + reg) >> 8;
}

static inline u16 o2scr_read(struct o2scr_info *info, int reg)
{
	return ioread16(info->mem + reg);
}

static inline void o2scr_write(struct o2scr_info *info, int reg, u16 val)
{
	return iowrite16(val, info->mem + reg);
}

static inline void o2scr_writel(struct o2scr_info *info, int reg, u64 val)
{
	o2scr_write(info, reg, val >> 16);
	o2scr_write(info, reg + 2, val & 0xffff);
}

static int o2scr_cmd(struct o2scr_info *info, u16 cmd)
{
	unsigned long tout;

	o2scr_write(info, EXE, cmd);
	for (tout = 50 * 1000; tout; tout --) {
		if (o2scr_read(info, STATUS_IT) & END_EXE) {
			o2scr_write(info, DEVAL_IT, ~END_EXE);
			break;
		}
		msleep(1);
	}

	return tout ? 0 : -ETIMEDOUT;
}

static int o2scr_clear_fifo(struct o2scr_info *info)
{
	int ret;

	if (o2scr_read(info, FIFO_NB) == 0)
		return 0;

	pr_debug("fifo not empty, clearing\n");
	ret = o2scr_cmd(info, RST_FIFO_EXE);
	if (ret)
		return ret;

	if (o2scr_read(info, STATUS_EXCH) & FIFO_EMPTY)
		return 0;

	return -ETIMEDOUT;
}

static int o2scr_read_atr(struct o2scr_info *info)
{
	int i;

	int flen = o2scr_read(info, FIFO_NB);
	pr_debug("FifoNB = %d\n", flen);
	flen &= 0x1ff;

	if (flen > sizeof(info->atr)) {
		pr_debug("ATR buf too small!\n");
		return -EINVAL;
	}

	info->atr_len = flen;
	for (i = 0; i < flen; i++) {
		info->atr[i] = o2scr_inb(info, FIFO_OUT);
	}

	pr_debug("ATR =");
	for (i = 0; i < flen; i++)
		pr_debug(" %02x", info->atr[i]);

	return 0;
}

static int o2scr_pon(struct o2scr_info *info)
{
	int ret = o2scr_cmd(info, PON_EXE);
	if (ret)
		return ret;

	pr_debug("st %04x", o2scr_read(info, STATUS_EXCH));

	if (o2scr_read(info, STATUS_EXCH) & 0xF700)
		return -ENXIO; /* synch cards not supported */

	return o2scr_read_atr(info);
}

static int o2scr_poff(struct o2scr_info *info)
{
	u16 status;
	int ret;

	status = o2scr_read(info, STATUS_EXCH);
	if (!(status & CRD_INS) || !(status & CRD_ON))
		return 0;

	ret = o2scr_cmd(info, POF_EXE);
	if (ret)
		return ret;

	msleep(1);

	if (o2scr_read(info, STATUS_EXCH) & CRD_ON)
		return -EIO;

	return 0;
}

static int o2scr_reset(struct o2scr_info *info)
{
	o2scr_clear_fifo(info);

	o2scr_write(info, MASK_IT, SCP| SCI | CLK_IT | ETU_IT | IT_REC | END_EXE);
	o2scr_write(info, MODE, o2scr_read(info, MODE) & ~CRD_DET);

	o2scr_write(info, EXE, RESET_EXE);
	msleep(1);
	o2scr_write(info, EXE, 0);
	msleep(1);

	o2scr_write(info, MODE, o2scr_read(info, MODE) | EDC | ATR_TO);

	o2scr_writel(info, BWT, 0xfa00);

	o2scr_clear_fifo(info);

	o2scr_write(info, FRQ_MODE, 4 << 4);

	o2scr_write(info, O2_POWER_DELAY_REG, 0xb00b);

	return 0;
}

irqreturn_t o2scr_interrupt(int irq, void *dev_id)
{
	struct o2scr_info *info = dev_id;
	u16 st = o2scr_read(info, STATUS_IT);

	if ((o2scr_read(info, MASK_IT) & st) == 0)
		return IRQ_NONE;

	pr_debug("interrupt\n");

	o2scr_write(info, DEVAL_IT, ~st);

	return IRQ_HANDLED;
}


static long o2scr_dev_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct o2scr_info *info = file->private_data;
	u16 reg;
	unsigned long ret;
	void __user *argp = (void __user *) arg;

	pr_debug("ioctl %x", cmd);
	switch (cmd) {
	case O2SCR_RESET:
		return o2scr_reset(info);
	case O2SCR_STATUS:
		BUILD_BUG_ON(sizeof(u16) != _IOC_SIZE(O2SCR_STATUS));
		reg = o2scr_read(info, STATUS_EXCH);
		return put_user(reg, (u16 __user *)argp);
	case O2SCR_PON:
		return o2scr_pon(info);
	case O2SCR_POFF:
		return o2scr_poff(info);
	case O2SCR_GET_ATR:
		ret = copy_to_user((u8 __user *)argp, info->atr, info->atr_len);
		return ret ? -EFAULT : info->atr_len;
	default:
		return -ENOTTY;
	}
}

static int o2scr_dev_open(struct inode *inode, struct file *file)
{
	struct o2scr_info *info = container_of(inode->i_cdev,
			struct o2scr_info, char_dev);

	if (test_and_set_bit_lock(O2SCR_DEV_BUSY, &info->flags))
		return -EBUSY;

	file->private_data = info;

	/* XXX: enable irq ? */

	return 0;
}

static int o2scr_dev_release(struct inode *inode, struct file *file)
{
	struct o2scr_info *info = file->private_data;

	/* XXX: disable IRQ */

	clear_bit_unlock(O2SCR_DEV_BUSY, &info->flags);

	return 0;
}

static const struct file_operations o2scr_dev_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.open		= o2scr_dev_open,
	.release	= o2scr_dev_release,
	.unlocked_ioctl	= o2scr_dev_ioctl,
};

static void o2scr_device_release(struct device *dev)
{
	struct o2scr_info *info = container_of(dev, struct o2scr_info, dev);

	mutex_lock(&idr_lock);
	idr_remove(&o2scr_idr, info->id);
	mutex_unlock(&idr_lock);
	kfree(info);
}

int __devinit o2scr_dev_add(struct o2scr_info *info, struct device *parent)
{
	int id;
	int err;

	if (idr_pre_get(&o2scr_idr, GFP_KERNEL) == 0) {
		return -ENOMEM;
	}

	mutex_lock(&idr_lock);
	err = idr_get_new(&o2scr_idr, NULL, &id);
	mutex_unlock(&idr_lock);
	if (err < 0)
		goto err;

	info->id = id;

	info->dev.parent = parent;
	info->dev.class = o2scr_class;
	info->dev.release = o2scr_device_release;

	dev_set_name(&info->dev, "o2scr%d", id);

	info->dev.devt = MKDEV(MAJOR(o2scr_devt), id);

	cdev_init(&info->char_dev, &o2scr_dev_fops);
	info->char_dev.owner = THIS_MODULE;

	err = device_register(&info->dev);
	if (err)
		goto err_idr;

	cdev_add(&info->char_dev, info->dev.devt, 1);

	o2scr_reset(info);

	return 0;

err_idr:
	mutex_lock(&idr_lock);
	idr_remove(&o2scr_idr, id);
	mutex_unlock(&idr_lock);
err:
	return err;
}

void __devexit o2scr_dev_remove(struct o2scr_info *info)
{
	cdev_del(&info->char_dev);
	device_unregister(&info->dev);
}

int __init o2scr_dev_init(void)
{
	int err;

	o2scr_class = class_create(THIS_MODULE, "o2scr");
	if (IS_ERR(o2scr_class)) {
		err = PTR_ERR(o2scr_class);
		goto err_class;
	}

	err = alloc_chrdev_region(&o2scr_devt, 0, O2SCR_DEV_MAX, "o2scr");
	if (err < 0)
		goto err_chrdev;

	return err;

err_chrdev:
	class_destroy(o2scr_class);
err_class:
	return err;
}
void o2scr_dev_exit(void)
{
	unregister_chrdev_region(o2scr_devt, O2SCR_DEV_MAX);
	class_destroy(o2scr_class);
}

