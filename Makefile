CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

BUILD = build
OPENSSL_BIN ?= openssl
ADMIN_SIGN_KEY ?=
DEV_SIGN_KEY ?=

CFLAGS = -ffreestanding -nostdlib -Wall -O2 -nostartfiles -fno-builtin -mgeneral-regs-only -Iinclude -Ithird_party/ed25519/src
LDFLAGS = -T linker.ld

# ---------------------------
# KERNEL SOURCES ONLY
# ---------------------------
C_SOURCES = \
kernel/kernel.c \
kernel/uart.c \
kernel/shell.c \
kernel/memory.c \
kernel/string.c \
kernel/process.c \
kernel/smp.c \
kernel/mailbox.c \
kernel/framebuffer.c \
kernel/console.c \
kernel/sd.c \
kernel/loader.c \
kernel/api.c \
kernel/fat32.c \
kernel/gpio.c \
kernel/clock.c \
kernel/emmc.c \
kernel/sdhost.c \
kernel/debug.c \
kernel/cache.c \
kernel/spinlock.c \
kernel/interrupt.c \
kernel/timer.c \
kernel/mmu.c \
kernel/blockdev.c \
kernel/syscall.c \
kernel/kernel_verify.c \
kernel/ed25519_verify.c \
kernel/net.c \
kernel/net_proto.c \
kernel/sha256.c \
kernel/arp.c \
kernel/ipv4.c \
kernel/icmp.c \
kernel/udp.c \
kernel/tcp.c \
kernel/socket.c \
kernel/trust.c \
kernel/nic_stub.c \
kernel/nic_smsc95xx.c \
kernel/usb_host.c \
third_party/ed25519/src/verify.c \
third_party/ed25519/src/ge.c \
third_party/ed25519/src/sc.c \
third_party/ed25519/src/fe.c \
third_party/ed25519/src/sha512.c


ASM_SOURCES = \
boot/boot.S \
kernel/context.S \
kernel/vectors.S


# ---------------------------
# OBJECTS
# ---------------------------
OBJS = \
$(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) \
$(patsubst %.S,$(BUILD)/%.o,$(ASM_SOURCES))

# ---------------------------
# DEFAULT TARGET
# ---------------------------
all: provisioned-kernel

check-signing-inputs:
	@if [ -z "$(ADMIN_SIGN_KEY)" ]; then echo "ADMIN_SIGN_KEY is required (Ed25519 private key path)"; exit 1; fi
	@if [ -z "$(DEV_SIGN_KEY)" ]; then echo "DEV_SIGN_KEY is required (Ed25519 private key path)"; exit 1; fi

trust-keys-header:
	python3 tools/gen_trust_keys_header.py include/trust_keys_autogen.h "$(ADMIN_SIGN_KEY)" "$(DEV_SIGN_KEY)" "$(OPENSSL_BIN)"

# Generate kernel manifest header from current build.
manifest-header: kernel8.img
	python3 tools/gen_kernel_manifest.py $(BUILD)/kernel8.elf kernel8.img include/kernel_manifest_autogen.h $(CROSS)nm 0x1 "$(ADMIN_SIGN_KEY)" "$(OPENSSL_BIN)"

# Two-pass build:
# 1) build kernel image
# 2) generate manifest header from image
# 3) rebuild so embedded manifest matches generated values
provisioned-kernel: check-signing-inputs trust-keys-header kernel8.img
	$(MAKE) manifest-header
	rm -f $(BUILD)/kernel/kernel_verify.o $(BUILD)/kernel8.elf kernel8.img
	$(MAKE) kernel8.img

# ---------------------------
# LINK STEP
# ---------------------------
kernel8.img: $(OBJS)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $(BUILD)/kernel8.elf $(OBJS)
	$(OBJCOPY) $(BUILD)/kernel8.elf -O binary kernel8.img

# ---------------------------
# GENERIC COMPILE RULES
# ---------------------------
$(BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------
# CLEAN
# ---------------------------
clean:
	rm -rf $(BUILD) kernel8.img
