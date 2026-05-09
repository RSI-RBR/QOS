CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

PROGRAM_MEMORY_BYTES ?= 16777216
GAME_SRC_DIR ?= src
GAME_ENTRY ?= gui_main.c
GAME_EXCLUDE ?= test_main.c
GAME_ENTRY_SOURCE = $(GAME_SRC_DIR)/$(GAME_ENTRY)
GAME_ALL_SOURCES = $(wildcard $(GAME_SRC_DIR)/*.c)
GAME_EXCLUDE_PATHS = $(addprefix $(GAME_SRC_DIR)/,$(GAME_ENTRY) $(GAME_EXCLUDE))
GAME_SOURCES ?= $(filter-out $(GAME_EXCLUDE_PATHS),$(GAME_ALL_SOURCES))
GAME_BUILD_OBJS = $(patsubst $(GAME_SRC_DIR)/%.c,game_%.o,$(GAME_SOURCES))
GAME_ENTRY_OBJ = game_entry.o
USER_LIB_OBJS = qos_user_heap.o qos_stdio.o qos_math.o qos_random.o
SDL_OBJS = sdl_shim.o
SIGN_KEY ?=
PQ_SIGN_KEY ?=
OPENSSL_BIN ?= openssl

COMMON_CFLAGS = -ffreestanding -nostdlib -O2 -g -fno-omit-frame-pointer -pie -fno-plt -fvisibility=hidden -fno-builtin -DQOS_USERSPACE -DQF_QOS_BUILD -DQOS_PROGRAM_MEMORY_BYTES=$(PROGRAM_MEMORY_BYTES) -I../../include -I$(GAME_SRC_DIR)
GAME_CFLAGS = $(COMMON_CFLAGS) -include qos_stdio.h
GAME_ENTRY_CFLAGS = $(GAME_CFLAGS) -Dmain=qf2d_main
QOS_LIB_CFLAGS = $(COMMON_CFLAGS) -DQOS_STDIO_NO_MACROS

all: game.bin

program_main.o: program_main.c
	$(CC) $(COMMON_CFLAGS) -c program_main.c -o program_main.o

start.o: ../hello/start.S
	$(CC) $(COMMON_CFLAGS) -c ../hello/start.S -o start.o

qos_user_heap.o: ../lib/qos_user_heap.c
	$(CC) $(COMMON_CFLAGS) -c ../lib/qos_user_heap.c -o qos_user_heap.o

qos_stdio.o: ../lib/qos_stdio.c
	$(CC) $(QOS_LIB_CFLAGS) -c ../lib/qos_stdio.c -o qos_stdio.o

qos_math.o: ../lib/qos_math.c
	$(CC) $(COMMON_CFLAGS) -c ../lib/qos_math.c -o qos_math.o

qos_random.o: ../lib/qos_random.c
	$(CC) $(COMMON_CFLAGS) -c ../lib/qos_random.c -o qos_random.o

sdl_shim.o: ../sdl_compat/sdl_shim.c
	$(CC) $(COMMON_CFLAGS) -c ../sdl_compat/sdl_shim.c -o sdl_shim.o

$(GAME_ENTRY_OBJ): $(GAME_ENTRY_SOURCE)
	$(CC) $(GAME_ENTRY_CFLAGS) -c $< -o $@

game_%.o: $(GAME_SRC_DIR)/%.c
	$(CC) $(GAME_CFLAGS) -c $< -o $@

game.elf: start.o program_main.o $(USER_LIB_OBJS) $(SDL_OBJS) $(GAME_ENTRY_OBJ) $(GAME_BUILD_OBJS)
	@if [ ! -f "$(GAME_ENTRY_SOURCE)" ]; then echo "Game entry source not found: $(GAME_ENTRY_SOURCE). Set GAME_ENTRY=... or GAME_SRC_DIR=..."; exit 1; fi
	@if [ -z "$(GAME_ALL_SOURCES)" ]; then echo "No private game sources found in $(GAME_SRC_DIR). Copy QuantumFront2D sources there or pass GAME_SRC_DIR=..."; exit 1; fi
	$(LD) -pie -T ../hello/linker.ld start.o program_main.o $(USER_LIB_OBJS) $(SDL_OBJS) $(GAME_ENTRY_OBJ) $(GAME_BUILD_OBJS) -o game.elf

game.raw: game.elf
	$(OBJCOPY) game.elf -O binary game.raw

game.bin: game.raw game.elf
	@if [ -z "$(SIGN_KEY)" ]; then echo "SIGN_KEY is required for signed build"; exit 1; fi
	QOS_PROGRAM_MEMORY_BYTES=$(PROGRAM_MEMORY_BYTES) python3 ../hello/build_program.py game.raw game.bin "GAME    BIN" 0x00010001 "$(SIGN_KEY)" "$(OPENSSL_BIN)" "$(PQ_SIGN_KEY)" game.pqs game.elf "$(CROSS)nm"

clean:
	rm -f *.o *.elf *.raw *.bin *.pqs
