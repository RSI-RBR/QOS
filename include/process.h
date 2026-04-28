#ifndef PROCESS_H
#define PROCESS_H

#include "uart.h"
#include "loader.h"

#define MAX_PROCESSES 8
#define STACK_SIZE 4096

typedef struct{
    unsigned long regs[12];
    void* sp;
    void* stack;
    program_entry_t entry;
    int active;
    int pid;
} process_t;

void *alloc_stack(void);

void free_stack(void *stack);

void process_init(void);

int process_create(program_entry_t entry);

void process_exit(int pid);


process_t* get_process(int pid);

process_t* get_current_process(void);

process_t* scheduler_next(void);

void schedule(void);

#endif
