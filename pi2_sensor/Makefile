obj-m:= ultra_switch.o

KDIR:= /lib/modules/$(shell uname -r)/build
ARM:= ARCH=arm
PWD:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) $(ARM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(ARM) clean

