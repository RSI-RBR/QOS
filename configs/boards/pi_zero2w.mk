# Raspberry Pi Zero 2 W target. Shares the BCM2837 platform with Pi 3,
# but uses the onboard CYW43 Wi-Fi path instead of LAN9514 Ethernet.
QOS_ARCH := aarch64
QOS_SOC := bcm2837
QOS_BOARD := pi_zero2w

LINKER ?= linker/pi_zero2w.ld
KERNEL_IMAGE ?= kernel8.img
KERNEL_ELF ?= $(BUILD)/kernel8.elf
KERNEL_PQS ?= kernel8.pqs

CFLAGS += -DQOS_BOARD_PI_ZERO2W=1 -DQOS_SOC_BCM2837=1 -DQOS_HAS_CYW43=1
