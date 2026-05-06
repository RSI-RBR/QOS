#ifndef PROCESS_H
#define PROCESS_H

#include "uart.h"
#include "loader.h"

#define MAX_PROCESSES 8
#define STACK_SIZE 16384
#define MAX_CPU_CORES 4

typedef enum {
    PROC_DEAD = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_REAPING
} process_state_t;

typedef struct{
    unsigned long regs[12];
    void* sp;
    void* stack;
    void* user_sp;
    program_entry_t entry;
    void* program_memory;
    unsigned long program_size;
    int program_heap_alloc;
    int user_mode;
    unsigned int owner_core;
    process_state_t state;
    unsigned long wake_tick;
    int pid;
} process_t;

void *alloc_stack(void);

void free_stack(void *stack);

void process_init(void);

int process_create(program_entry_t entry);
int process_create_loaded(loaded_program_t prog);

void process_exit(int pid);

void process_exit_current(void);
void process_fault_current(void);

process_t* get_process(int pid);
int process_current_pid(void);

process_t* get_current_process(void);

process_t* scheduler_next(void);

void schedule(void);

void scheduler_tick(void);
void scheduler_request_resched_core(unsigned int core_id);
int scheduler_consume_need_resched(void);
void scheduler_run_once(void);
void process_yield(void);
int scheduler_has_runnable(void);
void* scheduler_on_irq(void* irq_frame_sp);
void process_sleep(unsigned int ms);
void process_dump(void);
__attribute__((noreturn)) void process_enter_idle_loop(void);

#endif
