#ifndef LOADER_H
#define LOADER_H

#include "program.h"
#include "memory.h"
#include "uart.h"
#include "sd.h"
#include "fat32.h"
#include "cache.h"

typedef void (*program_entry_t)(void);

typedef struct{
    program_entry_t entry;
    void* memory;
    unsigned long size;
    unsigned long user_rw_offset;
    unsigned long user_rw_size;
    int heap_allocated;
} loaded_program_t;


void* alloc_program_memory(unsigned int size);
void loader_free_program_memory(void* ptr, unsigned long size);
void* loader_user_stack_top(void* program_base, unsigned long user_rw_offset, unsigned long user_rw_size);
void loader_mmu_init_pool(void);

loaded_program_t load_program_from_sd(void);
loaded_program_t load_program_from_sd_named(const char* fat_name_83);
int loader_is_busy(void);

void execute_program(unsigned long entry_addr);


#endif
