#include "uart.h"
#include "shell.h"
#include "memory.h"


void memzero(unsigned long start, unsigned long size){
    for (unsigned long i = 0; i < size; i++)
        ((char*)start)[i] = 0;
}

void kernel_main(void){
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
