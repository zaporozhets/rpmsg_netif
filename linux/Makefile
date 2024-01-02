

obj-m += rpmsg_eth.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	
	
	
clean:
	rm -f .tmp
	rm -f *.o*
	rm -f Mo*
	rm -f *.cmd
	rm -f mccmulti.mod*
	rm -fr .t* .mcc*
	rm -f *.ko
	rm -f *.mod.*
	rm -f *~
	rm -f .*

