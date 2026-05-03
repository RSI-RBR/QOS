#include "cache.h"

static unsigned long cache_line_size(void){
    unsigned long ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    unsigned long dminline = (ctr >> 16) & 0xFUL;
    return 4UL << dminline;
}

void clean_data_cache(void){
    asm volatile("dsb ish\n");
}

void invalidate_instruction_cache(void){
    asm volatile("ic iallu\n" "dsb ish\n" "isb\n");
}

void clean_invalidate_data_cache_range(unsigned long start, unsigned long size){
    if (size == 0){
        return;
    }
    unsigned long line = cache_line_size();
    unsigned long end = start + size;
    unsigned long addr = start & ~(line - 1UL);
    for (; addr < end; addr += line){
        asm volatile("dc civac, %0" : : "r"(addr) : "memory");
    }
    asm volatile("dsb ish" : : : "memory");
}
