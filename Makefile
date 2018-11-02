fl2000-y := \
		fl2000_drv.o \
		fl2000_registers.o \
		fl2000_interrupt.o \
		fl2000_streaming.o \
		fl2000_i2c.o \
		fl2000_drm.o

obj-m := fl2000.o

KSRC = /lib/modules/$(shell uname -r)/build

all:	modules

modules:
	make -C $(KSRC) M=$(PWD) modules

clean:
	make -C $(KSRC) M=$(PWD) clean
	rm -f $(PWD)/Module.symvers $(PWD)/*.ur-safe
