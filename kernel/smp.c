#include "smp.h"

#define LOCAL_BASE 0x40000000UL
#define CORE1_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0x9C))
#define CORE2_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xAC))
#define CORE3_MBOX3_SET (*(volatile unsigned int*)(LOCAL_BASE + 0xBC))
#define LOCAL_MAILBOX_INT_CONTROL(core) (*(volatile unsigned int*)(LOCAL_BASE + 0x50 + ((core) * 4u)))
#define LOCAL_MAILBOX0_SET(core) (*(volatile unsigned int*)(LOCAL_BASE + 0x80 + ((core) * 0x10u)))
#define LOCAL_MAILBOX0_CLR(core) (*(volatile unsigned int*)(LOCAL_BASE + 0xC0 + ((core) * 0x10u)))

// armstub8 secondary spin-table slots (64-bit entry addresses).
#define SPIN_CPU1 (*(volatile unsigned long*)0x000000E0UL)
#define SPIN_CPU2 (*(volatile unsigned long*)0x000000E8UL)
#define SPIN_CPU3 (*(volatile unsigned long*)0x000000F0UL)

extern void _start(void);
static volatile unsigned int g_smp_online_mask = 0;

static void atomic_or_u32(volatile unsigned int* addr, unsigned int bits){
    unsigned int oldv;
    unsigned int newv;
    unsigned int st;
    do{
        asm volatile("ldaxr %w0, [%1]" : "=&r"(oldv) : "r"(addr) : "memory");
        newv = oldv | bits;
        asm volatile("stlxr %w0, %w1, [%2]" : "=&r"(st) : "r"(newv), "r"(addr) : "memory");
    } while (st != 0);
}

static void atomic_and_u32(volatile unsigned int* addr, unsigned int bits){
    unsigned int oldv;
    unsigned int newv;
    unsigned int st;
    do{
        asm volatile("ldaxr %w0, [%1]" : "=&r"(oldv) : "r"(addr) : "memory");
        newv = oldv & bits;
        asm volatile("stlxr %w0, %w1, [%2]" : "=&r"(st) : "r"(newv), "r"(addr) : "memory");
    } while (st != 0);
}

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

void smp_mark_core_online(unsigned int core_id){
    if (core_id > 31u){
        return;
    }
    atomic_or_u32(&g_smp_online_mask, 1u << core_id);
}

void smp_mark_core_offline(unsigned int core_id){
    if (core_id > 31u){
        return;
    }
    atomic_and_u32(&g_smp_online_mask, ~(1u << core_id));
    asm volatile("dmb ishst" : : : "memory");
    asm volatile("sev" : : : "memory");
}

unsigned int smp_online_mask(void){
    return g_smp_online_mask;
}

void smp_init_ipi_for_core(unsigned int core_id){
    if (core_id >= 4u){
        return;
    }
    // Enable mailbox0 as IRQ source on this core.
    LOCAL_MAILBOX_INT_CONTROL(core_id) |= (1u << 0);
    asm volatile("dmb ishst" : : : "memory");
}

void smp_send_ipi(unsigned int target_core){
    if (target_core >= 4u){
        return;
    }
    // Mailbox interrupt remains asserted while any bit is set.
    LOCAL_MAILBOX0_SET(target_core) = 1u;
    asm volatile("dmb ishst" : : : "memory");
    asm volatile("sev" : : : "memory");
}

void smp_clear_ipi_for_core(unsigned int core_id){
    if (core_id >= 4u){
        return;
    }
    unsigned int pending = LOCAL_MAILBOX0_CLR(core_id);
    if (pending == 0u){
        pending = 1u;
    }
    LOCAL_MAILBOX0_CLR(core_id) = pending;
    asm volatile("dmb ishst" : : : "memory");
}
