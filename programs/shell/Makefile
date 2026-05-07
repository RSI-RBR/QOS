CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -O2 -pie -mgeneral-regs-only -DQOS_USERSPACE -I../../include
SIGN_KEY ?=
PQ_SIGN_KEY ?=
OPENSSL_BIN ?= openssl

all: shell.bin

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

shell.elf: main.o start.o
	$(LD) -pie -T linker.ld start.o main.o -o shell.elf

shell.raw: shell.elf
	$(OBJCOPY) shell.elf -O binary shell.raw

shell.bin: shell.raw
	@if [ -z "$(SIGN_KEY)" ]; then echo "SIGN_KEY is required for signed build"; exit 1; fi
	python3 build_program.py shell.raw shell.bin "SHELL   BIN" 0x1 "$(SIGN_KEY)" "$(OPENSSL_BIN)" "$(PQ_SIGN_KEY)" shell.pqs

clean:
	rm -f *.o *.elf *.raw *.bin *.pqs
