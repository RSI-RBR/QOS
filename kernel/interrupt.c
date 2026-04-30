#include "interrupt.h"
#include "uart.h"
#include "timer.h"
#include "process.h"

extern void vectors(void);

void interrupt_init(void){
    asm volatile("msr VBAR_EL1, %0" : : "r"(vectors));
    asm volatile("isb");

    timer_init();

    uart_puts("Interrupts initialized\n");
}

void enable_interrupts(void){
    asm volatile("msr daifclr, #2");
}

void disable_interrupts(void){
    asm volatile("msr daifset, #2");
}

void* irq_handler(void* irq_frame_sp){
    timer_clear_interrupt();
    timer_handler();
    return scheduler_on_irq(irq_frame_sp);
}

void sync_exception_handler(unsigned long esr, unsigned long elr, unsigned long spsr){
    uart_puts("\nSYNC EXCEPTION\n");
    uart_puts("ESR_EL1=");
    uart_puthex((unsigned int)esr);
    uart_puts("\nELR_EL1=");
    uart_puthex((unsigned int)elr);
    uart_puts("\nSPSR_EL1=");
    uart_puthex((unsigned int)spsr);
    uart_puts("\nHALTING\n");
    while (1){}
}
