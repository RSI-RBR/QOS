#ifndef LOADER_H
#define LOADER_H


#include "api.h"
#include "program.h"
#include "memory.h"
#include "uart.h"
#include "sd.h"
#include "fat32.h"
#include "api.h"
#include "cache.h"

typedef void (*program_entry_t)(kernel_api_t *);

void* alloc_program_memory(unsigned int size);

program_entry_t load_program_from_sd(void);

void execute_program(unsigned long entry_addr);


#endif
