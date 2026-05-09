CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -O2 -pie -fno-builtin -mgeneral-regs-only -DQOS_USERSPACE -I../../include
USER_LIB_OBJS = qos_user_heap.o
SIGN_KEY ?=
PQ_SIGN_KEY ?=
OPENSSL_BIN ?= openssl

all: webbrowser.bin

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

qos_user_heap.o: ../lib/qos_user_heap.c
	$(CC) $(CFLAGS) -c ../lib/qos_user_heap.c -o qos_user_heap.o

webbrowser.elf: main.o start.o $(USER_LIB_OBJS)
	$(LD) -pie -T linker.ld start.o main.o $(USER_LIB_OBJS) -o webbrowser.elf

webbrowser.raw: webbrowser.elf
	$(OBJCOPY) webbrowser.elf -O binary webbrowser.raw

webbrowser.bin: webbrowser.raw webbrowser.elf
	@if [ -z "$(SIGN_KEY)" ]; then echo "SIGN_KEY is required for signed build"; exit 1; fi
	python3 build_program.py webbrowser.raw webbrowser.bin "WEBBROWSBIN" 0x1 "$(SIGN_KEY)" "$(OPENSSL_BIN)" "$(PQ_SIGN_KEY)" webbrowser.pqs webbrowser.elf "$(CROSS)nm"

clean:
	rm -f *.o *.elf *.raw *.bin *.pqs
