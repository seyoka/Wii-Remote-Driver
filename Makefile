# Kernel module name
obj-m += wii-remote-mod.o

# Objects that make up the module
wii-remote-mod-objs := wii-remote-transport.o wii-remote-hid.o wii-remote-driver.o

# Build module using the kernel build system
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# Clean up compiled files
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

