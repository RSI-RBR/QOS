#include "process.h"

static unsigned char stacks[MAX_PROCESSES][STACK_SIZE];
static int used[MAX_PROCESSES] = {0};

static process_t processes[MAX_PROCESSES];

static int current_pid = -1;

extern void context_switch(void *old_sp, void *new_sp);


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
            processes[i].sp = stack;
            processes[i].active = 1;

            for (int r = 0; r < 12; r++){
                processes[i].regs[r] = 0;
            }

            return i;
        }
    }
    return -1;
}


void process_exit(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES) return;

    if (processes[pid].active){
        free_stack(processes[pid].stack);
        processes[pid].active = 0;
    }

    return;
}

void process_exit_current(void){
    if (current_pid < 0 || current_pid >= MAX_PROCESSES) return;
    if (processes[current_pid].active){
        free_stack(processes[current_pid].stack);
        processes[current_pid].active = 0;
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

    for (int i = 0; i < MAX_PROCESSES; i++){
        int next = (current_pid + i) % MAX_PROCESSES;
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
    process_t* old = get_current_process();
    process_t* next = scheduler_next();

    if (!next || old == next) return;

    context_switch(old, next);

}


