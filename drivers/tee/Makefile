# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_TEE) += tee.o
tee-objs += tee_core.o
tee-objs += tee_shm.o
tee-objs += tee_shm_pool.o
obj-$(CONFIG_OPTEE) += optee/
obj-$(CONFIG_TRUSTONIC_TEE_SUPPORT) += gud/
obj-$(CONFIG_MICROTRUST_TEE_SUPPORT) += teei/
obj-$(CONFIG_TRUSTKERNEL_TEE_SUPPORT) += tkcore/


# TEEGRIS kernel driver
#ccflags-$(CONFIG_TZDEV) += -I$(srctree)/drivers/tee/tzdev/include
#obj-$(CONFIG_TZDEV)		+= tzdev/