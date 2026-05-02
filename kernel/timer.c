#include "timer.h"
#include "process.h"
#include "cpu.h"

#define TIMER_INTERVAL 200000
#define LOCAL_BASE 0x40000000UL
#define CORE_TIMER_IRQCNTL(core) (*(volatile unsigned int*)(LOCAL_BASE + 0x40 + ((core) * 4u)))
#define CORE_CNTPNSIRQ (1u << 1)

void timer_init(void){
    unsigned int core = cpu_get_id();
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 1000;

    // Route EL1 physical timer interrupt to this core's IRQ path.
    CORE_TIMER_IRQCNTL(core) |= CORE_CNTPNSIRQ;

    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));

}

void timer_clear_interrupt(void){
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 1000;

    asm volatile("msr cntp_tval_el0, %0" : : "r"(interval));
}

volatile unsigned long system_ticks = 0;

void timer_handler(void){
    system_ticks ++;
    scheduler_tick();
}
