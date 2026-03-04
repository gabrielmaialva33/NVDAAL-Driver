obj-m += nvdaal_test.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod nvdaal_test.ko

unload:
	sudo rmmod nvdaal_test

test: all load
	dmesg | tail -50
	sudo rmmod nvdaal_test
