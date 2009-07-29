#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/idr.h>

#include "o2scr.h"

static struct class *o2scr_class;
static dev_t o2scr_devt;
#define O2SCR_DEV_MAX 4

static DEFINE_IDR(o2scr_idr);
static DEFINE_MUTEX(idr_lock);

static const struct file_operations o2scr_dev_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
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

