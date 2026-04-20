#include "uart.h"
#include "shell.h"
#include "memory.h"

#include "process.h"
#include "context.h"
#include "programs.h"

extern kernel_api_t kapi;


//extern void vectors(void);
#define USER_PROGRAM_ADDR 0x400000

static void cmd_runbin(int argc, char **argv){
    extern unsigned char _binary_hello_bin_start[];
    extern unsigned char _binary_hello_bin_end[];

    unsigned char *src = _binary_hello_bin_start;
    unsigned char *dst = (unsigned char*)USER_PROGRAM_ADDR;

    while (src < _binary_hello_bin_end){
        *dst++ = src++;
    }

    uart_puts("Running program...\n");

    void (*prog)(kernel_api_t *) = (void*)USER_PROGRAM_ADDR;

    void *stack = alloc_stack();

    run_program(prog, stack, &kapi);

    uart_puts("Program returned!\n");

    free_stack(stack);
}

void memzero(unsigned long start, unsigned long size){
    for (unsigned long i = 0; i < size; i++)
        ((char*)start)[i] = 0;
}

void kernel_main(void){
//    asm volatile("msr VBAR_EL1, %0" :: "r"(&vectors));

    uart_init();
    uart_puts("Uart initialized!\n");
    memory_init();

    uart_puts("Kernel booted successfully! \n");

    shell_init();
    shell_run();

    while(1){
        char c = uart_getc();
        uart_send(c);
    }

}
