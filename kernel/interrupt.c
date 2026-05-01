#include "interrupt.h"
#include "uart.h"
#include "timer.h"
#include "process.h"

extern void vectors(void);

#define LOCAL_BASE 0x40000000UL
#define CORE0_IRQ_SOURCE (*(volatile unsigned int*)(LOCAL_BASE + 0x60))
#define CORE0_CNTPNSIRQ_PENDING (1u << 1)

#define IRQ_BASE 0x3F00B000UL
#define IRQ_PENDING_2 (*(volatile unsigned int*)(IRQ_BASE + 0x204))
#define IRQ_SDHOST_PENDING_BIT (1u << 30) // IRQ 62 -> bank2 bit 30

static void (*g_sdhost_irq_handler)(void) = 0;

void interrupt_init(void){
    asm volatile("msr VBAR_EL1, %0" : : "r"(vectors));
    asm volatile("isb");

    timer_init();

    uart_puts("Interrupts initialized\n");
}

void interrupt_register_sdhost_irq(void (*handler)(void)){
    g_sdhost_irq_handler = handler;
}

void enable_interrupts(void){
    asm volatile("msr daifclr, #2");
}

void disable_interrupts(void){
    asm volatile("msr daifset, #2");
}

void* irq_handler(void* irq_frame_sp){
    unsigned int local_src = CORE0_IRQ_SOURCE;
    if (local_src & CORE0_CNTPNSIRQ_PENDING){
        timer_clear_interrupt();
        timer_handler();
        return scheduler_on_irq(irq_frame_sp);
    }

    if ((IRQ_PENDING_2 & IRQ_SDHOST_PENDING_BIT) && g_sdhost_irq_handler){
        g_sdhost_irq_handler();
        return irq_frame_sp;
    }

    return irq_frame_sp;
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
