# MT6572 CONSYS out-of-tree modules (WMT/BTIF/BT — WiFi later).
# Build:  make            (against ../kernel/linux, ARM cross)
#         make KDIR=...   (other tree)
KDIR ?= $(CURDIR)/../kernel/linux
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
