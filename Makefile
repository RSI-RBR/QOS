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
# GENERIC COMPILE RULES
# ---------------------------
$(BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------
# USER PROGRAM BUILD (NO CONFLICTS)
# ---------------------------

PROGRAM = $(BUILD)/program

$(PROGRAM).o: user/program.c
	mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -Iinclude -c $< -o $@

$(PROGRAM).elf: $(PROGRAM).o
	$(LD) -Ttext=0x0 $< -o $@

$(PROGRAM).bin: $(PROGRAM).elf
	$(OBJCOPY) $< -O binary $@

$(BUILD)/program_bin.o: $(PROGRAM).bin
	$(LD) -r -b binary $< -o $@

# ---------------------------
# CLEAN
# ---------------------------
clean:
	rm -rf $(BUILD) kernel8.img
