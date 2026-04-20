CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

BUILD = build

CFLAGS = -ffreestanding -nostdlib -Wall -O2 -nostartfiles -fno-builtin -Iinclude
LDFLAGS = -T linker.ld

C_SOURCES = \
kernel/kernel.c \
kernel/uart.c \
kernel/shell.c \
kernel/memory.c \
kernel/string.c \
kernel/programs.c \
kernel/process.c


ASM_SOURCES = \
boot/boot.S \
kernel/context.S

OBJS = \
$(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) \
$(patsubst %.S,$(BUILD)/%.o,$(ASM_SOURCES))

all: kernel8.img

# LINK STEP
kernel8.img: $(OBJS)
	mkdir -p $(BUILD)
	$(LD) $(LDFLAGS) -o $(BUILD)/kernel8.elf $(OBJS)
	$(OBJCOPY) $(BUILD)/kernel8.elf -O binary kernel8.img

# COMPILE C
$(BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# COMPILE ASM
$(BUILD)/%.o: %.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/hello.o: user/hello.c
	$(CC) -ffreestanding -nostdlib -Iinclude -c $< -o $@

build/hello.bin: user/hello.c
	$(OBJCOPY) -O binary $< $@


build/hello_bin.o: build/hello.bin
	$(LD) -r -b binary $< -o $@

OBJS += build/hello_bin.o

clean:
	rm -rf $(BUILD) kernel8.img
