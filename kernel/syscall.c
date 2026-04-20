#include "uart.h"

void handle_sync(void){
    unsigned long num;

    asm volatile("mov %0, x0" : "=r"(num));

    switch (num){
        case 1:
            uart_puts("Hello from SVC!\n");
            break;
        default:
            uart_puts("Unknown syscall!\n");
            break;
    }
}
