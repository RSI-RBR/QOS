#include "loader.h"
#include "memory.h"
#include "uart.h"
#include "sd.h"

#define PROGRAM_MAX_SIZE (64 * 1024)
#define PROGRAM_ADDR 0x400000

#define PROGRAM_SECTOR 100
#define PROGRAM_SECTORS 64 // size 32KB

static unsigned char program_buffer[PROGRAM_MAX_SIZE];

void *load_program_from_sd(const char *path){
    uart_puts("Loading program from SD... \n");

    int size = sd_read_file(path, program_buffer, PROGRAM_MAX_SIZE);

    if (size <= 0){
        uart_puts("Failed to load program\n");
        return 0;
    }

    uart_puts("Program loaded, size: ");
    uart_puthex(size);
    uart_puts("\n");

    unsigned char *dst = (unsigned char*)PROGRAM_ADDR;

    for (int i = 0; i < size; i++){
        dst[i] = program_buffer[i];
    }

    uart_puts("Program copied to exec memory\n");

    return (void*)PROGRAM_ADDR;
}

//extern unsigned char _binary_build_program_bin_start[];
//extern unsigned char _binary_build_program_bin_end[];

void* load_program(const void *src, unsigned long size, void *dst){
    uart_puts("Loading program...\n");

    for (unsigned long i = 0; i < size; i++){
        ((unsigned char*)dst)[i] = ((unsigned char*)src)[i];
    }

    uart_puts("Program loaded\n");

    return 0;
}
