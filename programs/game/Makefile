CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -O2 -pie -mgeneral-regs-only -DQOS_USERSPACE -I../../include

all: game.bin

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

sdl_shim.o: sdl_shim.c
	$(CC) $(CFLAGS) -c sdl_shim.c -o sdl_shim.o

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

game.elf: main.o sdl_shim.o start.o
	$(LD) -pie -T linker.ld start.o main.o sdl_shim.o -o game.elf

game.raw: game.elf
	$(OBJCOPY) game.elf -O binary game.raw

game.bin: game.raw
	python3 build_program.py game.raw game.bin

clean:
	rm -f *.o *.elf *.raw *.bin
