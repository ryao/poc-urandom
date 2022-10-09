
poc_cs128p-objs = mod_cs128p.o
poc_cs256pp-objs = mod_cs256pp.o
poc_blake3-objs = blake3.o blake3_generic.o blake3_impl.o blake3_x86-64.o \
	blake3_sse41.o blake3_avx2.o mod_blake3.o

obj-m = poc_cs128p.o poc_cs256pp.o poc_blake3.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
