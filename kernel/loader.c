#include "loader.h"
#include "memory.h"
#include "uart.h"
#include "sd.h"
#include "fat32.h"
#include "api.h"


#define PROGRAM_MAX (8 * 1024)
#define PROGRAM_ADDR ((unsigned long)0x40000)

static unsigned char buffer[PROGRAM_MAX];


unsigned long load_program_from_sd(void)
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

    uart_puts("File read OK. \n");

    if (size < sizeof(program_header_t)){
        uart_puts("Invalid program (too small)");
        return 0;
    }

    program_header_t *hdr = (program_header_t*)buffer;

    uart_puts("MAGIC raw: ");
    uart_puthex(buffer[0]);
    uart_puthex(buffer[1]);
    uart_puthex(buffer[2]);
    uart_puthex(buffer[3]);
    uart_puts("\n");

    if (hdr->magic != QOS_MAGIC){
        uart_puts("Bad magic.\n");
        return 0;
    }

    uart_puts("Valid QOS magic!\n");

    unsigned int code_size = hdr->size;
    unsigned int entry_offset = hdr->entry_offset;

    if (code_size > PROGRAM_MAX){
        uart_puts("Program too large.\n");
        return 0;
    }

    unsigned char *src = buffer + sizeof(program_header_t);
    unsigned char *dst = (unsigned char*)PROGRAM_ADDR;


    for (unsigned int i = 0; i < code_size; i++){
        dst[i] = src[i];
    }

    unsigned long entry = PROGRAM_ADDR + entry_offset;

    return entry;
}

void execute_program(unsigned long entry_addr){
    extern kernel_api_t kapi;

    uart_puts("EXEC: jumping ...\n");

    asm volatile ("dsb sy");
    asm volatile ("isb");

    void (*entry_fn)(kernel_api_t*) = (void(*)(kernel_api_t*))entry_addr;

    entry_fn(&kapi);

    uart_puts("Program returned to kernel.\n");
}

