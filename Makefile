CC = aarch64-linux-gnu-gcc
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

CFLAGS = -ffreestanding -nostdlib -Wall -O2
LDFLAGS = -T linker.ld

OBJS = boot.o kernel.o uart.o shell.o memory.o string.o

all: kernel8.img

boot.o: boot.S
	$(CC) $(CFLAGS) -c boot.S -o boot.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

uart.o: uart.c
	$(CC) $(CFLAGS) -c uart.c -o uart.o

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o kernel.elf

kernel8.img: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel8.img

clean:
	rm -f *.o *.elf *.img
