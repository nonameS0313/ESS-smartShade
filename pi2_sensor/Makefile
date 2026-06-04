obj-m:= ultra_switch.o

KDIR:=~/linux-rpi
ARM:= ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi-
PWD:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) $(ARM) modules
	arm-linux-gnueabi-gcc test_app.c -o test_app -static
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(ARM) clean
	rm test_app
scp:
	scp $(obj-m:.o=.ko) mknod.sh test_app pi@10.10.10.12:~/
	