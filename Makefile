obj-m = o2scr_cs.o
KSRC ?= /lib/modules/`uname -r`/build
EXTRA_CFLAGS = -DDEBUG

all: modules

modules modules_install clean:
	make -C $(KSRC) $@ M=$(PWD)
