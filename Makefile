obj-m = o2scr_cs.o
o2scr_cs-y := o2scr_card.o o2scr_dev.o
KSRC ?= /lib/modules/`uname -r`/build
EXTRA_CFLAGS = -DDEBUG

all: modules

modules modules_install clean:
	make -C $(KSRC) $@ M=$(PWD)
