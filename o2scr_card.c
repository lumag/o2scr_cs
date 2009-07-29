#include <linux/kernel.h>
#include <linux/module.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>

#include "o2scr.h"

static irqreturn_t o2scr_interrupt(int irq, void *dev_id)
{
	pr_debug("interrupt\n");

#if 0
	/* Acknowledge interrupt to reader. */
	ack = inb(dev->io_base);
	ack &= ~OZSCR_IRQACK;
	outb(ack, dev->io_base);
#endif

	return IRQ_HANDLED;
}

#define CS_CHECK(fn, ret) \
do { last_fn = (fn); if ((last_ret = (ret)) != 0) goto cs_failed; } while (0)


static int __devinit o2scr_config_check(struct pcmcia_device *p_dev,
		cistpl_cftable_entry_t *cfg,
		cistpl_cftable_entry_t *dflt,
		unsigned int vcc,
		void *priv_data)
{
	win_req_t *req = priv_data;
	memreq_t map;
	map.Page = 0;

	if (cfg->index == 0)
		return -ENODEV;

	/* Use power settings for Vcc and Vpp if present */
	/*  Note that the CIS values need to be rescaled */
	if (cfg->vcc.present & (1<<CISTPL_POWER_VNOM)) {
		if (vcc != cfg->vcc.param[CISTPL_POWER_VNOM]/10000)
			return -ENODEV;
	} else if (dflt->vcc.present & (1<<CISTPL_POWER_VNOM)) {
		if (vcc != dflt->vcc.param[CISTPL_POWER_VNOM]/10000)
			return -ENODEV;
	}

	if (cfg->vpp1.present & (1<<CISTPL_POWER_VNOM))
		p_dev->conf.Vpp = cfg->vpp1.param[CISTPL_POWER_VNOM]/10000;
	else if (dflt->vpp1.present & (1<<CISTPL_POWER_VNOM))
		p_dev->conf.Vpp = dflt->vpp1.param[CISTPL_POWER_VNOM]/10000;

	/* IO window settings */
	if (cfg->io.nwin > 0)
		p_dev->io.BasePort1 = cfg->io.win[0].base;
	else if (dflt->io.nwin > 0)
		p_dev->io.BasePort1 = dflt->io.win[0].base;
	else
		return -ENODEV;

	/* This reserves IO space but doesn't actually enable it */
	if (pcmcia_request_io(p_dev, &p_dev->io) != 0)
		return -ENODEV;

	if (pcmcia_request_window(&p_dev, req, &p_dev->win) != 0)
		return -ENODEV;

	if (cfg->mem.nwin > 0) {
		map.CardOffset = cfg->mem.win[0].card_addr;
	} else if (dflt->mem.nwin > 0) {
		map.CardOffset = dflt->mem.win[0].card_addr;
	} else
		return -ENODEV;

	if (pcmcia_map_mem_page(p_dev->win, &map) != 0)
		return -ENODEV;

	return 0;
}

static int __devinit o2scr_config(struct pcmcia_device *p_dev)
{
	struct o2scr_info *info = p_dev->priv;

	tuple_t tuple;
	u_short buf[64];
	int last_fn, last_ret;
	win_req_t req;

	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleDataMax = sizeof(buf);
	tuple.TupleOffset = 0;

	tuple.Attributes = 0;
	tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;

	p_dev->io.NumPorts1 = 32;
	p_dev->io.Attributes1 = IO_DATA_PATH_WIDTH_16;
	p_dev->io.IOAddrLines = 5;
	p_dev->io.NumPorts2 = 0;

	/* Interrupt setup */
	p_dev->irq.Attributes = IRQ_TYPE_DYNAMIC_SHARING;
	p_dev->irq.IRQInfo1 = IRQ_LEVEL_ID;
	p_dev->irq.Handler = &o2scr_interrupt;
	p_dev->irq.Instance = p_dev->priv;

	/* General socket configuration */
	p_dev->conf.IntType = INT_MEMORY_AND_IO;

	/* Memory settings */
	req.Attributes = WIN_DATA_WIDTH_16 | WIN_MEMORY_TYPE_CM	| WIN_ENABLE;
	req.Size = 0x1000; /* 4K page */
	req.Base = 0;
	req.AccessSpeed = 0;

	last_ret = pcmcia_loop_config(p_dev, o2scr_config_check, &req);
	if (last_ret) {
		goto failed;
	}

	if (p_dev->conf.Attributes & CONF_ENABLE_IRQ)
		CS_CHECK(RequestIRQ, pcmcia_request_irq(p_dev, &p_dev->irq));

	CS_CHECK(RequestConfiguration, pcmcia_request_configuration(p_dev, &p_dev->conf));

	/* Finally, report what we've done */
	printk(KERN_INFO "o2scr sock %s: index 0x%02x",
		   dev_name(&p_dev->dev), p_dev->conf.ConfigIndex);
	if (p_dev->conf.Vpp)
		printk(KERN_CONT ", Vpp %d.%d", p_dev->conf.Vpp/10, p_dev->conf.Vpp%10);
	if (p_dev->conf.Attributes & CONF_ENABLE_IRQ)
		printk(KERN_CONT ", irq %d", p_dev->irq.AssignedIRQ);
	if (p_dev->io.NumPorts1)
		printk(KERN_CONT ", io 0x%04x-0x%04x", p_dev->io.BasePort1,
			   p_dev->io.BasePort1+p_dev->io.NumPorts1-1);
	if (p_dev->io.NumPorts2)
		printk(KERN_CONT " & 0x%04x-0x%04x", p_dev->io.BasePort2,
			   p_dev->io.BasePort2+p_dev->io.NumPorts2-1);
	if (p_dev->win)
		printk(KERN_CONT ", mem 0x%06lx-0x%06lx", req.Base,
			   req.Base+req.Size-1);
	printk("\n");

	info->mem = ioremap(req.Base, req.Size);
	info->io = ioport_map(p_dev->io.BasePort1, p_dev->io.NumPorts1);

	return 0;
cs_failed:
	cs_error(p_dev, last_fn, last_ret);
failed:
	pcmcia_disable_device(p_dev);
	return -ENODEV;
}

static int __devinit o2scr_probe(struct pcmcia_device *p_dev)
{
	struct o2scr_info *info;
	int ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	p_dev->priv = info;
	info->p_dev = p_dev;

	ret = o2scr_config(p_dev);
	if (ret)
		goto err_config;

	ret = o2scr_dev_add(info, &p_dev->dev);
	if (ret)
		goto err_dev;

	return 0;

err_dev:
	iounmap(info->mem);
	info->mem = NULL;
	ioport_unmap(info->io);
	info->io = NULL;

	pcmcia_disable_device(p_dev);
err_config:
	p_dev->priv = NULL;
	kfree(info);
	return ret;

}

static void __devexit o2scr_remove(struct pcmcia_device *p_dev)
{
	struct o2scr_info *info = p_dev->priv;

	iounmap(info->mem);
	info->mem = NULL;
	ioport_unmap(info->io);
	info->io = NULL;

	pcmcia_disable_device(p_dev);
	p_dev->priv = NULL;

	o2scr_dev_remove(info);
}

static struct pcmcia_device_id o2scr_ids[] = {
	PCMCIA_DEVICE_PROD_ID123("O2Micro", "SmartCardBus Reader", "V1.0",
				 0x97299583, 0xB8501BA9, 0xE611E659),
	PCMCIA_DEVICE_NULL,
};
MODULE_DEVICE_TABLE(pcmcia, o2scr_ids);

static struct pcmcia_driver o2scr_driver = {
	.owner		= THIS_MODULE,
	.drv.name	= "o2scr_cs",
	.id_table	= o2scr_ids,
	.probe		= o2scr_probe,
	.remove		= __devexit_p(o2scr_remove),
};

static int __init init_o2scr(void)
{
	int err;
	err = o2scr_dev_init();
	if (err)
		return err;

	err = pcmcia_register_driver(&o2scr_driver);
	if (err)
		goto err_driver;

	return 0;

err_driver:
	o2scr_dev_exit();
	return err;
}

static void __exit exit_o2scr(void)
{
	pcmcia_unregister_driver(&o2scr_driver);
	o2scr_dev_exit();
}

module_init(init_o2scr);
module_exit(exit_o2scr);
MODULE_LICENSE("GPL");
