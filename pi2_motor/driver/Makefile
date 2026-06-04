obj-m += stepper_motor.o
ccflags-y := -I$(src)

# Raspberry Pi 4 kernel module build
#
# Native build (on Pi, recommended — headers match running kernel):
#   make -C driver
#
# Cross-build from PC (WSL/Linux):
#   make -C driver ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#        KDIR=/path/to/rpi/linux
#
# 32-bit Pi OS:
#   make -C driver ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR=...
#
# KDIR must point to the Pi kernel build tree (same version as `uname -r` on Pi).
# On Pi:  KDIR=/lib/modules/$(uname -r)/build
# From PC: clone/copy kernel source or rsync from Pi:
#   rsync -avz pi@10.10.10.12:/lib/modules/$(uname -r)/build ./kernel-build

ARCH          ?= arm64
CROSS_COMPILE ?=

ifeq ($(ARCH),arm64)
  KERNEL_ARCH := arm64
else ifeq ($(ARCH),arm)
  KERNEL_ARCH := arm
else
  $(error ARCH must be arm64 or arm)
endif

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean install rpi4 rpi4-32

all:
	$(MAKE) -C $(KDIR) ARCH=$(KERNEL_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) ARCH=$(KERNEL_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) ARCH=$(KERNEL_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules_install

rpi4:
	$(MAKE) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=$(KDIR)

rpi4-32:
	$(MAKE) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR=$(KDIR)
