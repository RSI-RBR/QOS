#include "smp.h"

#define LOCAL_BASE 0x40000000UL
#define CORE1_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0x9C))
#define CORE2_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xAC))
#define CORE3_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xBC))

volatile unsigned int smp_boot_release_mask = 0;

extern void _start(void);

void smp_release_secondary_cores(void){
    unsigned int entry = (unsigned int)(unsigned long)&_start;

    // Program mailbox-3 wake entry for secondary cores.
    CORE1_MBOX3_SET = entry;
    CORE2_MBOX3_SET = entry;
    CORE3_MBOX3_SET = entry;

    // Allow cores 1..3 to leave boot park loop.
    smp_boot_release_mask = 0x0EU;
    asm volatile("dmb ishst" : : : "memory");
    asm volatile("sev");
}
