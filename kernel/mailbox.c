#include "mailbox.h"
#include "uart.h"

#define MMIO_BASE 0x3F000000
#define MBOX_BASE (MMIO_BASE + 0xB880)

#define MBOX_READ   (*(volatile unsigned int*)(MBOX_BASE + 0x0))
#define MBOX_STATUS (*(volatile unsigned int*)(MBOX_BASE + 0x18))
#define MBOX_WRITE  (*(volatile unsigned int*)(MBOX_BASE + 0x20))

#define MBOX_EMPTY 0x40000000
#define MBOX_FULL  0x80000000

#define MAILBOX_CHANNEL_PROP 8

volatile unsigned int mbox[36] __attribute__((aligned(16)));

static unsigned long cache_line_size(void){
    unsigned long ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    // DminLine is log2(words). bytes = 4 << DminLine.
    return 4UL << ((ctr >> 16) & 0xF);
}

static void clean_invalidate_dcache_range(unsigned long start, unsigned long size){
    unsigned long line = cache_line_size();
    unsigned long addr = start & ~(line - 1);
    unsigned long end = (start + size + line - 1) & ~(line - 1);

    for (; addr < end; addr += line){
        asm volatile("dc civac, %0" : : "r"(addr) : "memory");
    }
    asm volatile("dsb ish");
}

int mailbox_call(unsigned char ch){
    unsigned int r = ((unsigned int)((unsigned long)&mbox) & ~0xF) | (ch & 0xF);
    unsigned long mbox_addr = (unsigned long)&mbox[0];
    unsigned long mbox_size = sizeof(mbox);

    // Make request visible to GPU before ringing mailbox doorbell.
    clean_invalidate_dcache_range(mbox_addr, mbox_size);

    while (MBOX_STATUS & MBOX_FULL);
    MBOX_WRITE = r;

    while (1){
        while (MBOX_STATUS & MBOX_EMPTY);

        if (MBOX_READ == r){
            // Refresh CPU view of response written by GPU.
            clean_invalidate_dcache_range(mbox_addr, mbox_size);
            return mbox[1] == 0x80000000;
        }
    }
}

int mailbox_set_emmc_clock(unsigned int hz){
    uart_puts("MAILBOX: set EMMC clock\n");

    mbox[0] = 9 * 4;
    mbox[1] = 0;

    mbox[2] = 0x00038002;
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 1;
    mbox[6] = hz;

    mbox[7] = 0;
    mbox[8] = 0;

    if (mailbox_call(MAILBOX_CHANNEL_PROP)){
        uart_puts("MAILBOX: EMMC clock set OK\n");
        return 0;
    }
    return -1;
}
