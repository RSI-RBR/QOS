#include "process.h"
#include "memory.h"
#include "socket.h"
#include "console.h"
#include "cpu.h"

typedef struct {
    int pid[MAX_PROCESSES];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
} run_queue_t;

static unsigned char stacks[MAX_PROCESSES][STACK_SIZE];
static int used[MAX_PROCESSES] = {0};

static process_t processes[MAX_PROCESSES];
static int current_pid[MAX_CPU_CORES];
static int zombie_pid[MAX_CPU_CORES];
static run_queue_t runq[MAX_CPU_CORES];

extern void restore_context_and_eret(void* frame_sp);
extern volatile unsigned long system_ticks;

#define IRQ_FRAME_WORDS 34
#define IRQ_FRAME_SIZE (IRQ_FRAME_WORDS * sizeof(unsigned long))
#define IRQ_FRAME_ELR_IDX 31
#define IRQ_FRAME_SPSR_IDX 32
#define IRQ_FRAME_USER_SP_IDX 33
#define INITIAL_SPSR_EL1H 0x345
#define INITIAL_SPSR_EL0T 0x000

static unsigned int scheduler_core_id(void){
    unsigned int core = cpu_get_id();
    if (core >= MAX_CPU_CORES){
        return 0;
    }
    return core;
}

static int tick_reached(unsigned long now, unsigned long target){
    return (long)(now - target) >= 0;
}

static void runq_reset(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return;
    }
    runq[core].head = 0;
    runq[core].tail = 0;
    runq[core].count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++){
        runq[core].pid[i] = -1;
    }
}

static int runq_contains(unsigned int core, int pid){
    if (core >= MAX_CPU_CORES || pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }

    run_queue_t* q = &runq[core];
    for (unsigned int i = 0; i < q->count; i++){
        unsigned int idx = (q->head + i) % MAX_PROCESSES;
        if (q->pid[idx] == pid){
            return 1;
        }
    }
    return 0;
}

static void runq_enqueue(unsigned int core, int pid){
    if (core >= MAX_CPU_CORES || pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    run_queue_t* q = &runq[core];
    if (q->count >= MAX_PROCESSES){
        return;
    }
    if (runq_contains(core, pid)){
        return;
    }

    q->pid[q->tail] = pid;
    q->tail = (q->tail + 1) % MAX_PROCESSES;
    q->count++;
}

static int runq_dequeue_ready(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return -1;
    }

    run_queue_t* q = &runq[core];
    unsigned int checks = q->count;
    while (checks-- > 0 && q->count > 0){
        int pid = q->pid[q->head];
        q->pid[q->head] = -1;
        q->head = (q->head + 1) % MAX_PROCESSES;
        q->count--;

        if (pid < 0 || pid >= MAX_PROCESSES){
            continue;
        }
        if (processes[pid].state != PROC_READY){
            continue;
        }
        if (processes[pid].owner_core != core){
            continue;
        }
        return pid;
    }
    return -1;
}

static int runq_has_ready(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return 0;
    }
    run_queue_t* q = &runq[core];
    for (unsigned int i = 0; i < q->count; i++){
        unsigned int idx = (q->head + i) % MAX_PROCESSES;
        int pid = q->pid[idx];
        if (pid >= 0 && pid < MAX_PROCESSES &&
            processes[pid].state == PROC_READY &&
            processes[pid].owner_core == core){
            return 1;
        }
    }
    return 0;
}

static int is_pid_pending_zombie(int pid){
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        if (zombie_pid[core] == pid){
            return 1;
        }
    }
    return 0;
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
    processes[pid].owner_core = 0;
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
    console_owner_on_process_exit(pid);
    socket_close_all_for_pid(pid);
    clear_process_descriptor(pid);
}

static void mark_current_for_reap(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return;
    }
    int pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }
    if (processes[pid].state == PROC_DEAD){
        return;
    }

    zombie_pid[core] = pid;
    processes[pid].state = PROC_DEAD;
    processes[pid].wake_tick = 0;
}

static void reap_pending_zombie(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return;
    }
    int pid = zombie_pid[core];
    if (pid < 0){
        return;
    }
    if (pid == current_pid[core]){
        return;
    }
    reap_process_resources(pid);
    zombie_pid[core] = -1;
}

static process_t* scheduler_next_for_core(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return 0;
    }

    int next = runq_dequeue_ready(core);
    if (next < 0){
        return 0;
    }

    current_pid[core] = next;
    processes[next].state = PROC_RUNNING;
    return &processes[next];
}

void scheduler_tick(void){
    return;
}

void process_init(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        clear_process_descriptor(i);
    }
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        current_pid[core] = -1;
        zombie_pid[core] = -1;
        runq_reset(core);
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
    unsigned int owner_core = scheduler_core_id();

    for (int i = 0; i < MAX_PROCESSES; i++){
        if (is_pid_pending_zombie(i)){
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
            processes[i].owner_core = owner_core;
            processes[i].wake_tick = 0;

            for (int r = 0; r < 12; r++){
                processes[i].regs[r] = 0;
            }
            runq_enqueue(owner_core, i);
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
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }
    if (processes[pid].state == PROC_DEAD){
        return;
    }

    unsigned int owner_core = processes[pid].owner_core;
    if (owner_core >= MAX_CPU_CORES){
        owner_core = 0;
    }

    if (pid == current_pid[owner_core]){
        mark_current_for_reap(owner_core);
    } else{
        reap_process_resources(pid);
    }
}

void process_exit_current(void){
    unsigned int core = scheduler_core_id();
    int pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    mark_current_for_reap(core);
    // Immediate handoff: switch away without reaping on this same stack/frame.
    current_pid[core] = -1;

    process_t* next = scheduler_next_for_core(core);
    if (next){
        restore_context_and_eret(next->sp);
    }

    // No runnable task right now. Enable IRQs so timer can wake sleepers,
    // then idle until a future interrupt schedules work.
    asm volatile("msr daifclr, #2" : : : "memory");
    while (1){
        asm volatile("wfi");
    }
}

void process_fault_current(void){
    mark_current_for_reap(scheduler_core_id());
}

process_t* get_process(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }
    return &processes[pid];
}

int process_current_pid(void){
    return current_pid[scheduler_core_id()];
}

process_t* get_current_process(void){
    int pid = current_pid[scheduler_core_id()];
    if (pid < 0){
        return 0;
    }
    return &processes[pid];
}

process_t* scheduler_next(void){
    return scheduler_next_for_core(scheduler_core_id());
}

void schedule(void){
    scheduler_run_once();
}

void scheduler_run_once(void){
    unsigned int core = scheduler_core_id();
    reap_pending_zombie(core);

    if (current_pid[core] >= 0){
        return;
    }

    process_t* next = scheduler_next_for_core(core);
    if (!next){
        return;
    }

    restore_context_and_eret(next->sp);
}

void process_yield(void){
    unsigned int core = scheduler_core_id();
    int pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    processes[pid].state = PROC_READY;
    runq_enqueue(core, pid);
    asm volatile("wfi");
}

int scheduler_has_runnable(void){
    unsigned int core = scheduler_core_id();
    int pid = current_pid[core];
    if (pid >= 0 && pid < MAX_PROCESSES && processes[pid].state == PROC_RUNNING){
        return 1;
    }
    return runq_has_ready(core);
}

void* scheduler_on_irq(void* irq_frame_sp){
    unsigned int core = scheduler_core_id();
    int cur = current_pid[core];

    reap_pending_zombie(core);

    for (int i = 0; i < MAX_PROCESSES; i++){
        if (processes[i].state == PROC_SLEEPING && tick_reached(system_ticks, processes[i].wake_tick)){
            processes[i].state = PROC_READY;
            processes[i].wake_tick = 0;
            unsigned int owner_core = processes[i].owner_core;
            if (owner_core >= MAX_CPU_CORES){
                owner_core = 0;
            }
            runq_enqueue(owner_core, i);
        }
    }

    if (cur >= 0 && cur < MAX_PROCESSES){
        if (processes[cur].state == PROC_RUNNING){
            processes[cur].state = PROC_READY;
            runq_enqueue(core, cur);
        }
        processes[cur].sp = irq_frame_sp;
    } else{
        process_t* next = scheduler_next_for_core(core);
        if (next){
            return next->sp;
        }
        return irq_frame_sp;
    }

    process_t* next = scheduler_next_for_core(core);
    if (!next){
        if (cur >= 0 && cur < MAX_PROCESSES){
            if (processes[cur].state == PROC_SLEEPING){
                // No alternate runnable task exists. Avoid desynchronizing
                // current_pid/state; treat sleep as a no-op in this edge case.
                processes[cur].state = PROC_RUNNING;
                processes[cur].wake_tick = 0;
                return irq_frame_sp;
            }
            if (processes[cur].state == PROC_DEAD){
                current_pid[core] = -1;
            } else{
                processes[cur].state = PROC_RUNNING;
                current_pid[core] = cur;
            }
        } else{
            current_pid[core] = -1;
        }
        return irq_frame_sp;
    }

    reap_pending_zombie(core);
    return next->sp;
}

void process_sleep(unsigned int ms){
    unsigned int core = scheduler_core_id();
    int pid = current_pid[core];
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
    uart_puts("PID STATE CORE WAKE\n");
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
        uart_send((char)('0' + (processes[i].owner_core & 0xF)));
        uart_puts(" ");
        uart_puthex((unsigned int)processes[i].wake_tick);
        uart_puts("\n");
    }
    uart_puts("CURR ");
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        uart_send((char)('0' + (core & 0xF)));
        uart_puts("=");
        if (current_pid[core] < 0){
            uart_puts(".. ");
        } else{
            uart_send((char)('0' + current_pid[core]));
            uart_puts(" ");
        }
    }
    uart_puts("\n");
    uart_puts("ticks=");
    uart_puthex((unsigned int)system_ticks);
    uart_puts("\n");
}
