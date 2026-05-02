#include "smp.h"

#define LOCAL_BASE 0x40000000UL
#define CORE1_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0x9C))
#define CORE2_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xAC))
#define CORE3_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xBC))

// armstub8 secondary spin-table slots (64-bit entry addresses).
#define SPIN_CPU1 (*(volatile unsigned long*)0x000000E0UL)
#define SPIN_CPU2 (*(volatile unsigned long*)0x000000E8UL)
#define SPIN_CPU3 (*(volatile unsigned long*)0x000000F0UL)

volatile unsigned int smp_boot_release_mask = 0;

extern void _start(void);

void smp_release_secondary_cores(void){
    unsigned long entry64 = (unsigned long)&_start;
    unsigned int entry32 = (unsigned int)entry64;

    // Release path A: armstub8 spin-table (what Pi firmware commonly uses).
    SPIN_CPU1 = entry64;
    SPIN_CPU2 = entry64;
    SPIN_CPU3 = entry64;

    // Release path B: local mailbox-3 set registers (kept as compatibility path).
    CORE1_MBOX3_SET = entry32;
    CORE2_MBOX3_SET = entry32;
    CORE3_MBOX3_SET = entry32;

    // Allow cores 1..3 to leave boot park loop.
    smp_boot_release_mask = 0x0EU;
    asm volatile("dmb ishst" : : : "memory");
    asm volatile("sev");
}
