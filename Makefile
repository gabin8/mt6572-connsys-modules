# MT6572 CONSYS out-of-tree modules (WMT/BTIF/BT — WiFi later).
# Build:  make            (against ../kernel/linux, ARM cross)
#         make KDIR=...   (other tree)
KDIR ?= $(CURDIR)/../kernel/linux
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
# wlan_gen2 links against cfg80211 (=m in kernel config); build it first with
#   make -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=net/wireless modules
EXTRA_SYMS := $(KDIR)/net/wireless/Module.symvers

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
