#include "smp.h"

#define LOCAL_BASE 0x40000000UL
#define CORE1_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0x9C))
#define CORE2_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xAC))
#define CORE3_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xBC))

// armstub8 secondary spin-table slots (64-bit entry addresses).
#define SPIN_CPU1 (*(volatile unsigned long*)0x000000E0UL)
#define SPIN_CPU2 (*(volatile unsigned long*)0x000000E8UL)
#define SPIN_CPU3 (*(volatile unsigned long*)0x000000F0UL)

extern void _start(void);

static unsigned long cache_line_size(void){
    unsigned long ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4UL << ((ctr >> 16) & 0xFUL);
}

static void dcache_clean_poc(unsigned long start, unsigned long size){
    unsigned long line = cache_line_size();
    unsigned long addr = start & ~(line - 1UL);
    unsigned long end = (start + size + line - 1UL) & ~(line - 1UL);
    for (; addr < end; addr += line){
        asm volatile("dc cvac, %0" : : "r"(addr) : "memory");
    }
    asm volatile("dsb ishst" : : : "memory");
}

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

    // Ensure spin-table writes are visible to parked cores before wakeup.
    dcache_clean_poc(0x000000E0UL, 24UL);
    asm volatile("dmb ishst" : : : "memory");
    asm volatile("sev");
}
