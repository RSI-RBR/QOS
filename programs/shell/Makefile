CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

PROGRAM_MEMORY_BYTES ?= 4194304
CFLAGS = -ffreestanding -nostdlib -O2 -pie -fno-builtin -mgeneral-regs-only -DQOS_USERSPACE -DQOS_PROGRAM_MEMORY_BYTES=$(PROGRAM_MEMORY_BYTES) -I../../include
USER_LIB_OBJS = qos_user_heap.o
SIGN_KEY ?=
PQ_SIGN_KEY ?=
OPENSSL_BIN ?= openssl

all: shell.bin

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

qos_user_heap.o: ../lib/qos_user_heap.c
	$(CC) $(CFLAGS) -c ../lib/qos_user_heap.c -o qos_user_heap.o

shell.elf: main.o start.o $(USER_LIB_OBJS)
	$(LD) -pie -T linker.ld start.o main.o $(USER_LIB_OBJS) -o shell.elf

shell.raw: shell.elf
	$(OBJCOPY) shell.elf -O binary shell.raw

shell.bin: shell.raw shell.elf
	@if [ -z "$(SIGN_KEY)" ]; then echo "SIGN_KEY is required for signed build"; exit 1; fi
	QOS_PROGRAM_MEMORY_BYTES=$(PROGRAM_MEMORY_BYTES) python3 build_program.py shell.raw shell.bin "SHELL   BIN" 0x1 "$(SIGN_KEY)" "$(OPENSSL_BIN)" "$(PQ_SIGN_KEY)" shell.pqs shell.elf "$(CROSS)nm"

clean:
	rm -f *.o *.elf *.raw *.bin *.pqs
