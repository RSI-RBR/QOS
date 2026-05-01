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
} loaded_program_t;


void* alloc_program_memory(unsigned int size);

loaded_program_t load_program_from_sd(void);
int loader_is_busy(void);

void execute_program(unsigned long entry_addr);


#endif
