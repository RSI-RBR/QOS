#include "process.h"
#include "memory.h"

static unsigned char stacks[MAX_PROCESSES][STACK_SIZE];
static int used[MAX_PROCESSES] = {0};

static process_t processes[MAX_PROCESSES];

static int current_pid = -1;
static int zombie_pid = -1;

extern void restore_context_and_eret(void* frame_sp);
extern volatile unsigned long system_ticks;

#define IRQ_FRAME_WORDS 34
#define IRQ_FRAME_SIZE (IRQ_FRAME_WORDS * sizeof(unsigned long))
#define IRQ_FRAME_ELR_IDX 31
#define IRQ_FRAME_SPSR_IDX 32
#define IRQ_FRAME_USER_SP_IDX 33
#define INITIAL_SPSR_EL1H 0x345
#define INITIAL_SPSR_EL0T 0x000

static int tick_reached(unsigned long now, unsigned long target){
    return (long)(now - target) >= 0;
}

static void clear_process_descriptor(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }
    for (int r = 0; r < 12; r++){
        processes[pid].regs[r] = 0;
    }
    processes[pid].sp = 0;
    processes[pid].stack = 0;
    processes[pid].user_sp = 0;
    processes[pid].entry = 0;
    processes[pid].program_memory = 0;
    processes[pid].program_size = 0;
    processes[pid].program_heap_alloc = 0;
    processes[pid].user_mode = 0;
    processes[pid].wake_tick = 0;
    processes[pid].state = PROC_DEAD;
    processes[pid].pid = pid;
}

static void process_bootstrap(void){
    process_t* p = get_current_process();
    if (!p || !p->entry){
        process_exit_current();
        return;
    }

    p->entry();
    process_exit_current();
}

static void* build_initial_context_el1(void* stack_top){
    unsigned long* frame = (unsigned long*)((unsigned long)stack_top - IRQ_FRAME_SIZE);
    for (int i = 0; i < IRQ_FRAME_WORDS; i++){
        frame[i] = 0;
    }
    frame[IRQ_FRAME_ELR_IDX] = (unsigned long)process_bootstrap;
    frame[IRQ_FRAME_SPSR_IDX] = INITIAL_SPSR_EL1H;
    frame[IRQ_FRAME_USER_SP_IDX] = 0;
    return frame;
}

static void* build_initial_context_el0(void* stack_top, unsigned long entry, void* user_sp){
    unsigned long* frame = (unsigned long*)((unsigned long)stack_top - IRQ_FRAME_SIZE);
    for (int i = 0; i < IRQ_FRAME_WORDS; i++){
        frame[i] = 0;
    }
    frame[IRQ_FRAME_ELR_IDX] = entry;
    frame[IRQ_FRAME_SPSR_IDX] = INITIAL_SPSR_EL0T;
    frame[IRQ_FRAME_USER_SP_IDX] = (unsigned long)user_sp;
    return frame;
}

static void reap_process_resources(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    if (processes[pid].stack){
        free_stack(processes[pid].stack);
    }
    if (processes[pid].program_memory){
        if (processes[pid].program_heap_alloc){
            kfree_secure(processes[pid].program_memory, processes[pid].program_size);
        } else{
            volatile unsigned char* p = (volatile unsigned char*)processes[pid].program_memory;
            for (unsigned long i = 0; i < processes[pid].program_size; i++){
                p[i] = 0;
            }
            loader_free_program_memory(processes[pid].program_memory, processes[pid].program_size);
        }
    }
    clear_process_descriptor(pid);
}

static void mark_current_for_reap(void){
    if (current_pid < 0 || current_pid >= MAX_PROCESSES){
        return;
    }

    if (processes[current_pid].state == PROC_DEAD){
        return;
    }

    zombie_pid = current_pid;
    processes[current_pid].state = PROC_DEAD;
    processes[current_pid].wake_tick = 0;
}

void scheduler_tick(void){
    return;
}

void process_init(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        clear_process_descriptor(i);
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
        if (i == zombie_pid){
            continue;
        }
        if (processes[i].state == PROC_DEAD){
            void* stack = alloc_stack();
            if (!stack){
                uart_puts("No stack available.\n");
                return -1;
            }
            processes[i].entry = entry;
            processes[i].stack = stack;
            processes[i].sp = build_initial_context_el1(stack);
            processes[i].user_sp = 0;
            processes[i].state = PROC_READY;
            processes[i].program_memory = 0;
            processes[i].program_size = 0;
            processes[i].program_heap_alloc = 0;
            processes[i].user_mode = 0;
            processes[i].wake_tick = 0;

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

    void* user_sp = loader_user_stack_top(prog.memory);
    if (!user_sp){
        reap_process_resources(pid);
        return -1;
    }

    processes[pid].program_memory = prog.memory;
    processes[pid].program_size = prog.size;
    processes[pid].program_heap_alloc = prog.heap_allocated;
    processes[pid].user_mode = 1;
    processes[pid].user_sp = user_sp;
    processes[pid].sp = build_initial_context_el0(processes[pid].stack, (unsigned long)prog.entry, user_sp);
    return pid;
}

void process_exit(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES) return;

    if (processes[pid].state == PROC_DEAD) return;

    if (pid == current_pid){
        mark_current_for_reap();
    } else{
        reap_process_resources(pid);
    }

    return;
}

void process_exit_current(void){
    if (current_pid < 0 || current_pid >= MAX_PROCESSES){
        return;
    }
    mark_current_for_reap();
    // Immediate handoff: do not wait for timer tick to switch away.
    current_pid = -1;
    scheduler_run_once();
    while (1){ asm volatile("wfi"); }
}

void process_fault_current(void){
    mark_current_for_reap();
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
    int start = (current_pid < 0) ? 0 : current_pid + 1;
    for (int i = 0; i < MAX_PROCESSES; i++){
        int next = (start + i) % MAX_PROCESSES;
        if (processes[next].state == PROC_READY || processes[next].state == PROC_RUNNING){
            current_pid = next;
            processes[next].state = PROC_RUNNING;
            return &processes[next];
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

    restore_context_and_eret(next->sp);
}

void process_yield(void){
    if (current_pid < 0 || current_pid >= MAX_PROCESSES){
        return;
    }
    processes[current_pid].state = PROC_READY;
    asm volatile("wfi");
}

int scheduler_has_runnable(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if (processes[i].state == PROC_READY || processes[i].state == PROC_RUNNING){
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

    for (int i = 0; i < MAX_PROCESSES; i++){
        if (processes[i].state == PROC_SLEEPING && tick_reached(system_ticks, processes[i].wake_tick)){
            processes[i].state = PROC_READY;
            processes[i].wake_tick = 0;
        }
    }

    if (current_pid >= 0 && current_pid < MAX_PROCESSES){
        if (processes[current_pid].state == PROC_RUNNING){
            processes[current_pid].state = PROC_READY;
        }
        processes[current_pid].sp = irq_frame_sp;
    } else{
        process_t* next = scheduler_next();
        if (next){
            return next->sp;
        }
        return irq_frame_sp;
    }

    process_t* next = scheduler_next();
    if (!next){
        // Recovery path: if scheduler state was desynced and a non-current task
        // remained in RUNNING, convert it back to READY and try once more.
        for (int i = 0; i < MAX_PROCESSES; i++){
            if (i == current_pid){
                continue;
            }
            if (processes[i].state == PROC_RUNNING){
                processes[i].state = PROC_READY;
            }
        }
        next = scheduler_next();
    }
    if (!next){
        current_pid = -1;
        return irq_frame_sp;
    }
    return next->sp;
}

void process_sleep(unsigned int ms){
    int pid = current_pid;
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    processes[pid].wake_tick = system_ticks + ms;
    processes[pid].state = PROC_SLEEPING;

    // Block cooperatively until the timer IRQ path wakes us.
    while (processes[pid].state == PROC_SLEEPING){
        asm volatile("wfi");
    }
}

void process_dump(void){
    uart_puts("PID STATE WAKE\n");
    for (int i = 0; i < MAX_PROCESSES; i++){
        uart_send('0' + i);
        uart_puts(" ");
        switch (processes[i].state){
            case PROC_DEAD: uart_puts("DEAD "); break;
            case PROC_READY: uart_puts("READY "); break;
            case PROC_RUNNING: uart_puts("RUN "); break;
            case PROC_SLEEPING: uart_puts("SLEEP "); break;
            default: uart_puts("UNK "); break;
        }
        uart_puthex((unsigned int)processes[i].wake_tick);
        uart_puts("\n");
    }
    uart_puts("ticks=");
    uart_puthex((unsigned int)system_ticks);
    uart_puts("\n");
}


