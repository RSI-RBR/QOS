#include "timer.h"
#include "process.h"
#include "cpu.h"
#include "crypto.h"

#define TIMER_INTERVAL 200000
#define LOCAL_BASE 0x40000000UL
#define CORE_TIMER_IRQCNTL(core) (*(volatile unsigned int*)(LOCAL_BASE + 0x40 + ((core) * 4u)))
#define CORE_CNTPNSIRQ (1u << 1)

void timer_init(void){
    unsigned int core = cpu_get_id();
    unsigned long freq;
    unsigned long cntkctl;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    unsigned long interval = freq / 1000;

    /*
     * User programs use the architectural counter for high-resolution frame
     * pacing. Let EL0 read CNTPCT_EL0 directly so game loops do not need a
     * syscall for every sub-millisecond timing check.
     */
    asm volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
    cntkctl |= 1u; // EL0PCTEN: allow EL0 physical counter reads.
    asm volatile("msr cntkctl_el1, %0" : : "r"(cntkctl) : "memory");
    asm volatile("isb");

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
    if ((system_ticks & 0x3Fu) == 0u){
        unsigned long mix[3];
        unsigned long c = 0;
        asm volatile("mrs %0, cntpct_el0" : "=r"(c));
        mix[0] = system_ticks;
        mix[1] = c;
        mix[2] = (unsigned long)cpu_get_id();
        crypto_add_entropy(mix, (unsigned int)sizeof(mix));
    }
    scheduler_tick();
}
