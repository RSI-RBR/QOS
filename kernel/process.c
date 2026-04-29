#include "process.h"
#include "api.h"
#include "memory.h"

static unsigned char stacks[MAX_PROCESSES][STACK_SIZE];
static int used[MAX_PROCESSES] = {0};

static process_t processes[MAX_PROCESSES];

static int current_pid = -1;
static void* kernel_sp = 0;
static int zombie_pid = -1;

extern void context_switch(void **old_sp, void *new_sp);
extern kernel_api_t kapi;

static void process_bootstrap(void){
    process_t* p = get_current_process();
    if (!p || !p->entry){
        process_exit_current();
        return;
    }

    p->entry(&kapi);
    process_exit_current();
}

static void* build_initial_context(void* stack_top){
    unsigned long* frame = (unsigned long*)stack_top;
    frame -= 12;
    for (int i = 0; i < 12; i++){
        frame[i] = 0;
    }
    frame[1] = (unsigned long)process_bootstrap;
    return frame;
}

static void reap_process_resources(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    free_stack(processes[pid].stack);
    if (processes[pid].program_memory){
        kfree_secure(processes[pid].program_memory, processes[pid].program_size);
        processes[pid].program_memory = 0;
        processes[pid].program_size = 0;
    }
}

void scheduler_tick(void){
    return;
}

void process_init(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        processes[i].active = 0;
        processes[i].pid = i;
    }
}

void *alloc_stack(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if (!used[i]){
            used[i] = 1;

            for (int j = 0; j < STACK_SIZE; j++){
                stacks[i][j] = 0;
            }
            void *top = stacks[i] + STACK_SIZE;

            // align stack (important)
            top = (void*)((unsigned long)top & ~0xF);

            // give space for function prologue
            top -= 128;

            return top;
        }
    }
    return 0;
}

void free_stack(void *stack){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if ((void*)(stacks[i] + STACK_SIZE) == (void*)((unsigned long)stack + 128)){
            used[i] = 0;
            for (unsigned long s = 0; s < STACK_SIZE; s++){stacks[i][s] = 0;}
            return;
        }
    }
}

int process_create(program_entry_t entry){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if (!processes[i].active){
            void* stack = alloc_stack();
            if (!stack){
                uart_puts("No stack available.\n");
                return 0;
            }
//            void* top = stacks[i] + STACK_SIZE;
//            top = (void*)((unsigned long)top & ~0xF);
            processes[i].entry = entry;
            processes[i].stack = stack;
            processes[i].sp = build_initial_context(stack);
            processes[i].active = 1;
            processes[i].program_memory = 0;
            processes[i].program_size = 0;

//            processes[i].program_memory = prog_mem;
//            processes[i].program_size = prog_size;

            for (int r = 0; r < 12; r++){
                processes[i].regs[r] = 0;
            }

            return i;
        }
    }
    return -1;
}

int process_create_loaded(loaded_program_t prog){
    int pid = process_create(prog.entry);
    if (pid < 0){
        return pid;
    }

    processes[pid].program_memory = prog.memory;
    processes[pid].program_size = prog.size;
    return pid;
}

void process_exit(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES) return;

    if (!processes[pid].active) return;
    processes[pid].active = 0;
    reap_process_resources(pid);

    return;
}

void process_exit_current(void){
    process_t* current = get_current_process();
    if (current_pid < 0 || current_pid >= MAX_PROCESSES) return;
    if (processes[current_pid].active){
        zombie_pid = current_pid;
        processes[zombie_pid].active = 0;
        current_pid = -1;
    } else{
        current_pid = -1;
    }
    if (kernel_sp){
        context_switch(&current->sp, kernel_sp);
    }
    return;
}

process_t* get_process(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES) return 0;
    return &processes[pid];
}

process_t* get_current_process(void){
    if (current_pid < 0) return 0;
    return &processes[current_pid];
}

process_t* scheduler_next(void){
//    int next = (current + 1) % MAX_PROCESSES;
    int start = (current_pid < 0) ? 0 : current_pid + 1;
    for (int i = 0; i < MAX_PROCESSES; i++){
        int next = (start + i) % MAX_PROCESSES;
//        int idx = (next + i) % MAX_PROCESSES;
        if (processes[next].active){
            current_pid = next;
//            current = idx;

//            context_switch(&processes[prev].stack, processes[idx].stack);
            return &processes[next];;
        }
    }
    return 0;
}

//extern void context_switch(process_t *old, process_t *new);

void schedule(void){
    scheduler_run_once();
}

void scheduler_run_once(void){
    if (zombie_pid >= 0){
        reap_process_resources(zombie_pid);
        zombie_pid = -1;
    }

    process_t* next = scheduler_next();

    if (!next){
        return;
    }

    context_switch(&kernel_sp, next->sp);
}

void process_yield(void){
    process_t* current = get_current_process();
    if (!current || !kernel_sp){
        return;
    }
    context_switch(&current->sp, kernel_sp);
}

int scheduler_has_runnable(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if (processes[i].active){
            return 1;
        }
    }
    return 0;
}


