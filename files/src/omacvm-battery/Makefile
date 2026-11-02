# From try-omarchy (github.com/omacom/try-omarchy), MIT, renamed for OmacVM.
# Kbuild sets KERNELRELEASE and takes the first branch (DKMS too, see
# dkms.conf). By hand: make [KVER=<kernel release>].
ifneq ($(KERNELRELEASE),)
obj-m := omacvm_battery.o
omacvm_battery-y := omacvm-battery.o
else
KVER ?= $(shell uname -r)
KDIR ?= /usr/lib/modules/$(KVER)/build

.PHONY: modules clean

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
