#include "process.h"
#include "api.h"
#include "memory.h"

static unsigned char stacks[MAX_PROCESSES][STACK_SIZE];
static int used[MAX_PROCESSES] = {0};

static process_t processes[MAX_PROCESSES];

static int current_pid = -1;
static int zombie_pid = -1;
static int logged_program_start = 0;

extern kernel_api_t kapi;
extern void restore_context_and_eret(void* frame_sp);

#define IRQ_FRAME_WORDS 34
#define IRQ_FRAME_SIZE (IRQ_FRAME_WORDS * sizeof(unsigned long))
#define IRQ_FRAME_ELR_IDX 31
#define IRQ_FRAME_SPSR_IDX 32
#define INITIAL_SPSR_EL1H 0x345

static void process_bootstrap(void){
    process_t* p = get_current_process();
    if (!p || !p->entry){
        uart_puts("process_bootstrap: invalid current process\n");
        process_exit_current();
        return;
    }

    uart_puts("process_bootstrap: enter pid=");
    uart_send('0' + p->pid);
    uart_puts("\n");
    p->entry(&kapi);
    uart_puts("process_bootstrap: returned pid=");
    uart_send('0' + p->pid);
    uart_puts("\n");
    process_exit_current();
}

static void* build_initial_context(void* stack_top){
    unsigned long* frame = (unsigned long*)((unsigned long)stack_top - IRQ_FRAME_SIZE);
    for (int i = 0; i < IRQ_FRAME_WORDS; i++){
        frame[i] = 0;
    }
    frame[IRQ_FRAME_ELR_IDX] = (unsigned long)process_bootstrap;
    frame[IRQ_FRAME_SPSR_IDX] = INITIAL_SPSR_EL1H;
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
    uart_puts("process_create: begin\n");
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

            uart_puts("process_create: pid=");
            uart_send('0' + i);
            uart_puts("\n");
            return i;
        }
    }
    uart_puts("process_create: no slot\n");
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
    if (current_pid < 0 || current_pid >= MAX_PROCESSES) return;
    if (processes[current_pid].active){
        zombie_pid = current_pid;
        processes[zombie_pid].active = 0;
    }
    while (1){ asm volatile("wfi"); }
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

    if (current_pid >= 0){
        return;
    }

    process_t* next = scheduler_next();

    if (!next){
        return;
    }

    unsigned long* frame = (unsigned long*)next->sp;
    uart_puts("scheduler_run_once: pid=");
    uart_send('0' + current_pid);
    uart_puts(" frame=");
    uart_puthex((unsigned int)(unsigned long)next->sp);
    uart_puts(" elr=");
    uart_puthex((unsigned int)frame[IRQ_FRAME_ELR_IDX]);
    uart_puts(" spsr=");
    uart_puthex((unsigned int)frame[IRQ_FRAME_SPSR_IDX]);
    uart_puts("\n");

    restore_context_and_eret(next->sp);
}

void process_yield(void){
    return;
}

int scheduler_has_runnable(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if (processes[i].active){
            return 1;
        }
    }
    return 0;
}

void* scheduler_on_irq(void* irq_frame_sp){
    if (zombie_pid >= 0){
        reap_process_resources(zombie_pid);
        zombie_pid = -1;
    }

    if (current_pid >= 0 && current_pid < MAX_PROCESSES){
        processes[current_pid].sp = irq_frame_sp;
    } else{
        return irq_frame_sp;
    }

    process_t* next = scheduler_next();
    if (!next){
        return irq_frame_sp;
    }
    if (!logged_program_start && next->pid > 0){
        logged_program_start = 1;
        uart_puts("scheduler_on_irq: switching to program pid=");
        uart_send('0' + next->pid);
        uart_puts("\n");
    }
    return next->sp;
}


