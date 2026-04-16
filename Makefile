ifneq ($(KERNELRELEASE),)

obj-m := my_ramdisk_mq_wq.o

else

K_DIR ?= /lib/modules/$(shell uname -r)/build

all:	
	$(MAKE) C=1 -C $(K_DIR) M=$(PWD) modules

clean:	
	$(MAKE) -C $(K_DIR) M=$(PWD) clean
	$(RM) *~

endif
