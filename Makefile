CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

BOARD ?= pi3
BOARD_CONFIG = configs/boards/$(BOARD).mk
ifeq ($(wildcard $(BOARD_CONFIG)),)
$(error Unknown BOARD '$(BOARD)'. Expected one of: pi3 pi_zero2w pi5)
endif

BUILD_ROOT ?= build
BUILD ?= $(BUILD_ROOT)/$(BOARD)
OPENSSL_BIN ?= openssl
ADMIN_SIGN_KEY ?=
DEV_SIGN_KEY ?=
ADMIN_PQ_PUB ?=
DEV_PQ_PUB ?=
ADMIN_PQ_SIGN_KEY ?=
DEV_PQ_SIGN_KEY ?=

# Backward-compatible aliases from older Lamport variable names.
# These map old env vars to the current PQ variable names.
ifneq ($(strip $(ADMIN_LAMPORT_PUB)),)
ADMIN_PQ_PUB := $(ADMIN_LAMPORT_PUB)
endif
ifneq ($(strip $(DEV_LAMPORT_PUB)),)
DEV_PQ_PUB := $(DEV_LAMPORT_PUB)
endif
ifneq ($(strip $(ADMIN_LAMPORT_SIGN_KEY)),)
ADMIN_PQ_SIGN_KEY := $(ADMIN_LAMPORT_SIGN_KEY)
endif
ifneq ($(strip $(DEV_LAMPORT_SIGN_KEY)),)
DEV_PQ_SIGN_KEY := $(DEV_LAMPORT_SIGN_KEY)
endif

CFLAGS = -ffreestanding -nostdlib -Wall -O2 -nostartfiles -fno-builtin -fstack-protector-strong -mgeneral-regs-only -DARGON2_NO_THREADS -Iinclude -Ithird_party/ed25519/src -Ithird_party/pqclean/common -Ithird_party/pqclean/crypto_sign/ml-dsa-65/clean -Ithird_party/pqclean/crypto_kem/ml-kem-768/clean -Ithird_party/pqclean/crypto_kem/kyber768/clean -Ithird_party/argon2_ref/include -Ithird_party/argon2_ref/src -Ithird_party/argon2_ref/src/blake2

include $(BOARD_CONFIG)

LDFLAGS = -T $(LINKER)

# ---------------------------
# KERNEL SOURCES ONLY
# ---------------------------
C_SOURCES = \
$(SOC_C_SOURCES) \
$(BOARD_C_SOURCES) \
kernel/kernel.c \
kernel/headless_control.c \
kernel/uart.c \
kernel/klog.c \
kernel/shell.c \
kernel/memory.c \
kernel/panic.c \
kernel/string.c \
kernel/libc_compat.c \
kernel/process.c \
kernel/smp.c \
kernel/mailbox.c \
kernel/framebuffer.c \
kernel/display.c \
kernel/gpu2d.c \
kernel/v3d.c \
kernel/fb_console.c \
kernel/console.c \
kernel/terminal.c \
kernel/sd.c \
kernel/loader.c \
kernel/api.c \
kernel/fat32.c \
kernel/sandbox_file.c \
kernel/gpio.c \
kernel/clock.c \
kernel/emmc.c \
kernel/sdhost.c \
kernel/debug.c \
kernel/cache.c \
kernel/dma.c \
kernel/crypto.c \
kernel/auth.c \
kernel/aes_gcm.c \
kernel/spinlock.c \
kernel/interrupt.c \
kernel/timer.c \
kernel/mmu.c \
kernel/blockdev.c \
kernel/syscall.c \
kernel/kernel_verify.c \
kernel/ed25519_verify.c \
kernel/pq_sig.c \
kernel/pq_kem.c \
kernel/pqclean_randombytes.c \
kernel/pqclean_alloc.c \
kernel/argon2_kdf.c \
kernel/net.c \
kernel/net_proto.c \
kernel/sha256.c \
kernel/rsa_verify.c \
kernel/ecdsa_verify.c \
kernel/ecc_curve_p256.c \
kernel/ecc_curve_p384.c \
kernel/arp.c \
kernel/ipv4.c \
kernel/icmp.c \
kernel/udp.c \
kernel/dhcp.c \
kernel/tcp.c \
kernel/x509_verify.c \
kernel/tls_record.c \
kernel/tls_key_schedule.c \
kernel/tls_handshake.c \
kernel/tls_session.c \
kernel/socket.c \
kernel/trust.c \
kernel/remote_login.c \
kernel/sdio_bus.c \
kernel/cyw43.c \
kernel/nic_cyw43.c \
kernel/nic_stub.c \
kernel/nic_smsc95xx.c \
kernel/usb_host.c \
kernel/x25519.c \
third_party/argon2_ref/src/core.c \
third_party/argon2_ref/src/ref.c \
third_party/argon2_ref/src/blake2/blake2b.c \
third_party/pqclean/common/fips202.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/ntt.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/packing.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/poly.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/polyvec.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/reduce.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/rounding.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/sign.c \
third_party/pqclean/crypto_sign/ml-dsa-65/clean/symmetric-shake.c \
third_party/ed25519/src/verify.c \
third_party/ed25519/src/ge.c \
third_party/ed25519/src/sc.c \
third_party/ed25519/src/fe.c \
third_party/ed25519/src/sha512.c

MLKEM768_CLEAN_SOURCES := $(wildcard third_party/pqclean/crypto_kem/ml-kem-768/clean/*.c)
KYBER768_CLEAN_SOURCES := $(wildcard third_party/pqclean/crypto_kem/kyber768/clean/*.c)
ifneq ($(strip $(MLKEM768_CLEAN_SOURCES)),)
C_SOURCES += $(MLKEM768_CLEAN_SOURCES)
CFLAGS += -DQOS_HAVE_PQCLEAN_MLKEM768
endif
ifneq ($(strip $(KYBER768_CLEAN_SOURCES)),)
C_SOURCES += $(KYBER768_CLEAN_SOURCES)
CFLAGS += -DQOS_HAVE_PQCLEAN_KYBER768
endif


ASM_SOURCES = \
$(SOC_ASM_SOURCES) \
$(BOARD_ASM_SOURCES) \
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

ca-roots-sync:
	python3 tools/sync_ca_roots.py

pq-kem-status:
	@echo "ML-KEM clean sources: $(words $(MLKEM768_CLEAN_SOURCES))"
	@echo "Kyber768 clean sources: $(words $(KYBER768_CLEAN_SOURCES))"
	@if [ "$(words $(MLKEM768_CLEAN_SOURCES))" -gt 0 ]; then echo "PQ KEM backend: ML-KEM-768"; \
	elif [ "$(words $(KYBER768_CLEAN_SOURCES))" -gt 0 ]; then echo "PQ KEM backend: Kyber768 (compat)"; \
	else echo "PQ KEM backend: unavailable"; fi

check-signing-inputs:
	@if [ -z "$(ADMIN_SIGN_KEY)" ]; then echo "ADMIN_SIGN_KEY is required (Ed25519 private key path)"; exit 1; fi
	@if [ -z "$(DEV_SIGN_KEY)" ]; then echo "DEV_SIGN_KEY is required (Ed25519 private key path)"; exit 1; fi

trust-keys-header:
	python3 tools/gen_trust_keys_header.py include/trust_keys_autogen.h "$(ADMIN_SIGN_KEY)" "$(DEV_SIGN_KEY)" "$(OPENSSL_BIN)" "$(ADMIN_PQ_PUB)" "$(DEV_PQ_PUB)"

# Generate kernel manifest header from current build.
manifest-header: $(KERNEL_IMAGE)
	python3 tools/gen_kernel_manifest.py $(KERNEL_ELF) $(KERNEL_IMAGE) include/kernel_manifest_autogen.h $(CROSS)nm 0x1 "$(ADMIN_SIGN_KEY)" "$(OPENSSL_BIN)" "$(ADMIN_PQ_SIGN_KEY)" $(KERNEL_PQS)

# Two-pass build:
# 1) build kernel image
# 2) generate manifest header from image
# 3) rebuild so embedded manifest matches generated values
provisioned-kernel: check-signing-inputs trust-keys-header $(KERNEL_IMAGE)
	$(MAKE) BOARD=$(BOARD) manifest-header
	rm -f $(BUILD)/kernel/kernel_verify.o $(KERNEL_ELF) $(KERNEL_IMAGE)
	$(MAKE) BOARD=$(BOARD) $(KERNEL_IMAGE)

# ---------------------------
# LINK STEP
# ---------------------------
$(KERNEL_IMAGE): $(OBJS)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $(KERNEL_ELF) $(OBJS)
	$(OBJCOPY) $(KERNEL_ELF) -O binary $(KERNEL_IMAGE)

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
	rm -rf $(BUILD) $(KERNEL_IMAGE) $(KERNEL_PQS)
