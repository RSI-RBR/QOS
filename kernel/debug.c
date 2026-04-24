#include "debug.h"

extern unsigned long stack_bottom;

void check_stack(void){
    unsigned long *guard = (unsigned long *)&stack_bottom;

    if (*guard != 0xAAAAAAAA){
        uart_puts("STACK CORRUPTION DETECTED!\n");

        uart_puts("Expected: ");
        uart_puthex(0xAAAAAAAA);

        uart_puts(" Got: ");
        uart_puthex(*guard);
        uart_puts("\n");
        return;
    }
}
