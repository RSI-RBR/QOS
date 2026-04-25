#ifndef LOADER_H
#define LOADER_H


#include "api.h"
#include "program.h"


typedef void (*program_entry_t)(kernel_api_t *);

unsigned long load_program_from_sd(void);

void execute_program(unsigned long entry_addr);


#endif
