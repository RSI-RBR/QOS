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
kernel/programs.c \
kernel/process.c \
kernel/task.c \
kernel/mailbox.c \
kernel/framebuffer.c \
kernel/sd.c \
kernel/loader.c \
kernel/api.c

ASM_SOURCES = \
boot/boot.S \
kernel/context.S

# ---------------------------
# OBJECTS
# ---------------------------
OBJS = \
$(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES)) \
$(patsubst %.S,$(BUILD)/%.o,$(ASM_SOURCES)) \
$(BUILD)/program_bin.o

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
# COMPILE C FILES
# ---------------------------
$(BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------
# COMPILE ASM FILES
# ---------------------------
$(BUILD)/%.o: %.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------
# USER PROGRAM BUILD PIPELINE
# ---------------------------

$(BUILD)/program.o: user/program.c
	mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -Iinclude -c $< -o $@

$(BUILD)/program.elf: $(BUILD)/program.o
	$(LD) -Ttext=0x0 $< -o $@

$(BUILD)/program.bin: $(BUILD)/program.elf
	$(OBJCOPY) $< -O binary $@

$(BUILD)/program_bin.o: $(BUILD)/program.bin
	$(LD) -r -b binary $< -o $@

# ---------------------------
# CLEAN
# ---------------------------
clean:
	rm -rf $(BUILD) kernel8.img# DEFAULT TARGET
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
# COMPILE C FILES
# ---------------------------
$(BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------
# COMPILE ASM FILES
# ---------------------------
$(BUILD)/%.o: %.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Build user program.

build/program.o: user/program.c
	$(CC) -ffreestanding -nostdlib -Iinclude -c $< -o $@

build/program.bin: build/program.o
	$(LD) -Ttext=0x0 build/program.o -o build/program.elf
	$(OBJCOPY) build/program.elf -O binary $@

build/program_bin.o: build/program.bin
	$(LD) -r -b binary $< -o $@

OBJS += build/program_bin.o

# ---------------------------
# CLEAN
# ---------------------------
clean:
	rm -rf $(BUILD) kernel8.img
