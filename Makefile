MODULE_NAME := hid-rmi

LINUXINCLUDE := -I$(PWD)/include $(LINUXINCLUDE)

obj-m			+= $(MODULE_NAME).o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install: $(MODULE_NAME).ko $(MODULE_NAME).mod.c
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules_install
	/bin/bash install.sh ${MODULE_NAME} hid-generic

uninstall:
	/bin/bash restore.sh $(MODULE_NAME)

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean

