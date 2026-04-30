#include "timer.h"
#include "process.h"
#include "uart.h"

#define TIMER_INTERVAL 200000
#define LOCAL_BASE 0x40000000UL
#define CORE0_TIMER_IRQCNTL (*(volatile unsigned int*)(LOCAL_BASE + 0x40))
#define CORE0_CNTPNSIRQ (1u << 1)

void timer_init(void){
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 1000;

    // Route EL1 physical timer interrupt to core0 IRQ path (Pi3 local controller).
    CORE0_TIMER_IRQCNTL |= CORE0_CNTPNSIRQ;

    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));

    uart_puts("timer_init: CNTPNSIRQ enabled\n");
}

void timer_clear_interrupt(void){
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 1000;

    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
}

volatile unsigned long system_ticks = 0;

void timer_handler(void){
    static int first_tick = 1;
    system_ticks ++;
    if (first_tick){
        first_tick = 0;
        uart_puts("timer_handler: first tick\n");
    }
    scheduler_tick();
}
