#include "loader.h"
#include "memory.h"
#include "uart.h"
#include "sd.h"

#define PROGRAM_MAX_SIZE (64 * 1024)
#define PROGRAM_ADDR 0x400000

#define PROGRAM_SECTOR 100
#define PROGRAM_SECTORS 64 // size 32KB

void *load_program_from_sd(void)
{
    uart_puts("Loading program from SD...\n");

    unsigned char *dst = (unsigned char*)PROGRAM_ADDR;

    // read directly into execution memory
    if (sd_read_blocks(PROGRAM_SECTOR, dst, PROGRAM_SECTORS) != 0) {
        uart_puts("SD read failed!\n");
        return 0;
    }

    uart_puts("Program loaded into memory\n");

    return (void*)PROGRAM_ADDR;
}
