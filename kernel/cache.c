#include "cache.h"

void clean_data_cache(void){
    asm volatile("dsb ish\n");
}

void invalidate_instruction_cache(void){
    asm volatile("ic iallu\n" "dsb ish\n" "isb\n");
}
