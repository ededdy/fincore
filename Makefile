# Rules to make fincore utility
# 
# Author: Sougata Santra <sougata.santra@gmail.com>

# Set this to change the name of the kernel module.  This will
# also change debug-fs path through which we communicate with
# between user-space and kernel space.
export CONFIG_FINCORE_MODULE_NAME = fincore

FINCORE_MODULE_NAME = $(CONFIG_FINCORE_MODULE_NAME)
FINCORE_DIR_PATH ?=/sys/kernel/debug/${FINCORE_MODULE_NAME}
CFLAGS += -DFINCORE_DIR_PATH=\"$(FINCORE_DIR_PATH)\"

KERNELDIR ?= /lib/modules/${RELEASE}/build
RELEASE := `uname -r`
PWD     := `pwd`

all:	kmodule fincore-tool

fincore-tool: fincore-u.c fincore-tool.c
	gcc -g -Wall ${CFLAGS} fincore-u.c fincore-tool.c -o fincore-tool
kmodule:
	${MAKE} -C ${KERNELDIR} M=${PWD}/kmod modules

.Phony: clean
clean:
	rm -f fincore-tool
	${MAKE} -C ${KERNELDIR} M=${PWD}/kmod clean

