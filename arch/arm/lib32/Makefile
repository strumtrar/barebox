# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_ARM_LINUX)	+= armlinux.o
obj-$(CONFIG_CMD_BOOTZ)	+= bootz.o
obj-$(CONFIG_BOOTM)     += bootm.o
obj-$(CONFIG_BOOTM_OPTEE) += start-kernel-optee.o
obj-$(CONFIG_CMD_BOOTU) += bootu.o
obj-$(CONFIG_BOOT_ATAGS) += atags.o
obj-y	+= div0.o
obj-y	+= findbit.o
obj-y	+= io.o
obj-y	+= io-readsb.o
obj-y	+= io-readsw-armv4.o
obj-y	+= io-readsl.o
obj-y	+= io-writesb.o
obj-y	+= io-writesw-armv4.o
obj-y	+= io-writesl.o
obj-y	+= lib1funcs.o
obj-y	+= ashrdi3.o
obj-y	+= ashldi3.o
obj-y	+= lshrdi3.o
obj-y	+= runtime-offset.o
pbl-y	+= runtime-offset.o
obj-$(CONFIG_ARM_OPTIMZED_STRING_FUNCTIONS)	+= memcpy.o
obj-$(CONFIG_ARM_OPTIMZED_STRING_FUNCTIONS)	+= memmove.o
obj-$(CONFIG_ARM_OPTIMZED_STRING_FUNCTIONS)	+= memset.o
obj-$(CONFIG_ARM_UNWIND) += unwind.o
obj-$(CONFIG_MODULES) += module.o
obj-$(CONFIG_ARM_MODULE_PLTS) += module-plts.o
extra-y += barebox.lds

pbl-y	+= lib1funcs.o
pbl-y	+= ashldi3.o
pbl-y	+= div0.o
pbl-$(CONFIG_CPU_32v7)	+= arm_architected_timer.o
CFLAGS_arm_architected_timer.o := -march=armv7-a

obj-pbl-y	+= setjmp.o

pbl-$(CONFIG_PBL_OPTEE)	+= optee-early.o
