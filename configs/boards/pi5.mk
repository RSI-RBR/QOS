# Raspberry Pi 5 target scaffold. The BCM2712/RP1 platform port is not
# enabled yet; this file reserves the target name and output shape.
QOS_ARCH := aarch64
QOS_SOC := bcm2712
QOS_BOARD := pi5

LINKER ?= linker/pi5.ld
KERNEL_IMAGE ?= kernel_2712.img
KERNEL_ELF ?= $(BUILD)/kernel_2712.elf
KERNEL_PQS ?= kernel_2712.pqs

CFLAGS += -DQOS_BOARD_PI5=1 -DQOS_SOC_BCM2712=1
