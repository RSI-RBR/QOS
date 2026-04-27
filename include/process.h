#ifndef PROCESS_H
#define PROCESS_H

#include "uart.h"
#include "loader.h"

#define MAX_PROCESSES 8
#define STACK_SIZE 4096

typedef struct{
    program_entry_t entry;
    void *stack;
    int active;
} process_t;

void *alloc_stack(void);
void free_stack(void *stack);

process_t* process_create(program_entry_t entry);
void process_destroy(process_t* p);

#endif
