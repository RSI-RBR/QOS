#include "loader.h"
#include "memory.h"
#include "uart.h"
#include "sd.h"
#include "fat32.h"


#define PROGRAM_MAX (256 * 1024)
#define PROGRAM_ADDR 0x400000

static unsigned char buffer[PROGRAM_MAX];

program_entry_t load_program_from_sd(void)
{
    uart_puts("Loading program from SD...\n");

    if (fat32_init()){
        uart_puts("FAT init failed.\n");
        return 0;
    }

    int size = fat32_read_file("PROGRAM BIN", buffer, PROGRAM_MAX);

    if (size <= 0){
        uart_puts("Load failed.\n");
        return 0;
    }

    uart_puts("Program loaded! \n");

    unsigned char *dst = (unsigned char*)PROGRAM_ADDR;

    for (int i = 0; i < size; i++){
        dst[i] = buffer[i];
    }

    return (program_entry_t)PROGRAM_ADDR;
}
