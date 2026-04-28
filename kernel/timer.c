#include "timer.h"

#define TIMER_INTERVAL 200000

void timer_init(void){
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 100;

    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));
}

void timer_clear_interrupt(void){
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 100;

    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
}
