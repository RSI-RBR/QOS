# Raspberry Pi 3 Model B/B+ target.
QOS_ARCH := aarch64
QOS_SOC := bcm2837
QOS_BOARD := pi3

LINKER ?= linker/pi3.ld
KERNEL_IMAGE ?= kernel8.img
KERNEL_ELF ?= $(BUILD)/kernel8.elf
KERNEL_PQS ?= kernel8.pqs

CFLAGS += -DQOS_BOARD_PI3=1 -DQOS_SOC_BCM2837=1
