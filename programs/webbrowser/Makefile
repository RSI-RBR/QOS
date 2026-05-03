CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -O2 -pie -mgeneral-regs-only -DQOS_USERSPACE -I../../include
SIGN_KEY ?=
OPENSSL_BIN ?= openssl

all: webbrowser.bin

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

webbrowser.elf: main.o start.o
	$(LD) -pie -T linker.ld start.o main.o -o webbrowser.elf

webbrowser.raw: webbrowser.elf
	$(OBJCOPY) webbrowser.elf -O binary webbrowser.raw

webbrowser.bin: webbrowser.raw
ifneq ($(strip $(SIGN_KEY)),)
	python3 build_program.py webbrowser.raw webbrowser.bin "WEBBROWSBIN" 0x1 "$(SIGN_KEY)" "$(OPENSSL_BIN)"
else
	python3 build_program.py webbrowser.raw webbrowser.bin "WEBBROWSBIN" 0x1
endif

clean:
	rm -f *.o *.elf *.raw *.bin
