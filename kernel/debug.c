#include "debug.h"

extern unsigned char stack_bottom;
extern unsigned char stack_top;

void check_stack(void){
    unsigned char *p = &stack_bottom;

    while (p < &stack_top){
        if (*p != 0xAA){
            uart_puts("STACK CORRUPTION DETECTED!\n");
            return;
        }
        p++;
    }

    uart_puts("Stack OK\n");
}
