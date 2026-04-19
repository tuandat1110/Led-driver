obj-m += bbb_glue.o
obj-m += button_driver.o
obj-m += led_driver.o

KDIR := /home/tuandat/kernel-source/KERNEL
CROSS_COMPILE := /home/tuandat/kernel-source/dl/gcc-15.2.0-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-
ARCH := arm
PWD := $(shell pwd)

all:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean