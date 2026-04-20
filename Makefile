CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = aarch64-linux-gnu-objcopy
BUILD = build

CFLAGS = -ffreestanding -nostdlib -Wall -O2 -nostartfiles -fno-builtin -Iinclude
LDFLAGS = -T linker.ld

C_SOURCES = kernel/kernel.c kernel/uart.c kernel/shell.c kernel/memory.c kernel/string.c

ASM_SOURCES = boot/boot.S

OBJS = $(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) $(patsubst %.S,$(BUILD)/%.o,$(ASM_SOURCES))

all: kernel8.img

kernel8.img: $(OBJS) $(LD) $(LDFLAGS) -o $(BUILD)/kernel8.elf $(OBJS) $(CROSS)objcopy $(BUILD)/kernel8.elf -O binary kernel8.img

$(BUILD)/%.o: %.c mkdir -p $(dir $@) $(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S mkdir -p $(dir $@) $(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) kernel8.img
