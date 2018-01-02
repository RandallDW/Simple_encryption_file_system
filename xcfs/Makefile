XCFS_VERSION="0.1"

EXTRA_CFLAGS += -DXCFS_VERSION=\"$(XCFS_VERSION)\"

obj-m += xcfs.o

xcfs-y := dentry.o file.o inode.o main.o super.o lookup.o mmap.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
