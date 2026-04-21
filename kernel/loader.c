#include "loader.h"
#include "memory.h"
#include "uart.h"

#define PROGRAM_ADDR 0x400000

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
