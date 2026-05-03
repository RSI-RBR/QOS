CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -O2 -pie -mgeneral-regs-only -DQOS_USERSPACE -I../../include
SIGN_KEY ?=
OPENSSL_BIN ?= openssl

all: program.bin

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

program.elf: main.o start.o
	$(LD) -pie -T linker.ld start.o main.o -o program.elf

program.raw: program.elf
	$(OBJCOPY) program.elf -O binary program.raw

program.bin: program.raw
	@if [ -z "$(SIGN_KEY)" ]; then echo "SIGN_KEY is required for signed build"; exit 1; fi
	python3 build_program.py program.raw program.bin "PROGRAM BIN" 0x00010001 "$(SIGN_KEY)" "$(OPENSSL_BIN)"

clean:
	rm -f *.o *.elf *.raw *.bin
