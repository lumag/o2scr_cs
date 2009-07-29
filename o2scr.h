#ifndef O2SCR_H
#define O2SCR_H

#include <linux/device.h>
#include <linux/cdev.h>

struct o2scr_info {
	struct pcmcia_device	*p_dev;
	struct device		dev;
	struct cdev		char_dev;
	int			id;
	void __iomem		*io;
	void __iomem		*mem;
};

int o2scr_dev_add(struct o2scr_info *info, struct device *parent);
void o2scr_dev_remove(struct o2scr_info *info);

int o2scr_dev_init(void);
void o2scr_dev_exit(void);

#endif
