#include "interrupt.h"
#include "uart.h"
#include "timer.h"

extern void vectors(void);

void interrupt_init(void){
    asm volatile("msr VBAR_EL1, %0" : : "r"(vectors));

    timer_init();

    uart_puts("Interrupts initialized\n");
}

void enable_interrupts(void){
    asm volatile("msr daifclr, #2");
}

void disable_interrupts(void){
    asm volatile("msr daifset, #2");
}

void irq_handler(void){
    timer_clear_interrupt();
    timer_handler();
}
