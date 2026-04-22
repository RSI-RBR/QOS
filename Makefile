CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

BUILD = build

CFLAGS = -ffreestanding -nostdlib -Wall -O2 -nostartfiles -fno-builtin -Iinclude
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
kernel/task.c \
kernel/mailbox.c \
kernel/framebuffer.c \
kernel/sd.c \
kernel/loader.c \
kernel/api.c \
kernel/fat32.c \
kernel/gpio.c \
kernel/clock.c \
kernel/emmc.c


ASM_SOURCES = \
boot/boot.S \
kernel/context.S

# ---------------------------
# OBJECTS
# ---------------------------
OBJS = \
$(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) \
$(patsubst %.S,$(BUILD)/%.o,$(ASM_SOURCES))

# ---------------------------
# DEFAULT TARGET
# ---------------------------
all: kernel8.img

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
