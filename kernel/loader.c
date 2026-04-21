#include "loader.h"
#include "memory.h"
#include "uart.h"

#define PROGRAM_ADDR 0x400000

extern unsigned char _binary_program_bin_start[];
extern unsigned char _binary_program_bin_end[];

void* load_program(){
    uart_puts("Loading program...\n");

    unsigned char *src = _binary_program_bin_start;
    unsigned char *dst = (unsigned char*)PROGRAM_ADDR;

    while (src < _binary_program_bin_end){
        *dst++ = *src++;
    }

    uart_puts("Program loaded\n");

    return (void*)PROGRAM_ADDR;
}
