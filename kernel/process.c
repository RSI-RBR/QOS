#include "process.h"
#include "memory.h"
#include "socket.h"
#include "tls_session.h"
#include "console.h"
#include "cpu.h"
#include "spinlock.h"
#include "smp.h"
#include "mmu.h"

typedef struct {
    int pid[MAX_PROCESSES];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
} run_queue_t;

typedef struct {
    int valid;
    int pid;
    void* stack;
    void* program_memory;
    unsigned long program_size;
    int program_heap_alloc;
} process_cleanup_t;

static unsigned char stacks[MAX_PROCESSES][STACK_SIZE];
static int used[MAX_PROCESSES] = {0};

static process_t processes[MAX_PROCESSES];
static int current_pid[MAX_CPU_CORES];
static int zombie_pid[MAX_CPU_CORES];
static unsigned int need_resched[MAX_CPU_CORES];
static run_queue_t runq[MAX_CPU_CORES];
static volatile int g_process_ready = 0;
static unsigned long sched_ticks[MAX_CPU_CORES];
static unsigned long double_run_blocked = 0;
static int sleep_head = -1;
static int sleep_next[MAX_PROCESSES];
static unsigned char sleep_in_queue[MAX_PROCESSES];
static spinlock_t g_process_lock;

extern void restore_context_and_eret(void* frame_sp);
extern volatile unsigned long system_ticks;

#define IRQ_FRAME_WORDS 34
#define IRQ_FRAME_SIZE (IRQ_FRAME_WORDS * sizeof(unsigned long))
#define IRQ_FRAME_ELR_IDX 31
#define IRQ_FRAME_SPSR_IDX 32
#define IRQ_FRAME_USER_SP_IDX 33
#define INITIAL_SPSR_EL1H 0x345
#define INITIAL_SPSR_EL0T 0x000

static void runq_enqueue(unsigned int core, int pid);
static void mark_need_resched_locked(unsigned int core_id);
static int is_pid_pending_zombie(int pid);
static int pid_running_on_other_core_locked(int pid, unsigned int core);

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

static void sleepq_remove_locked(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }
    if (!sleep_in_queue[pid]){
        return;
    }

    int prev = -1;
    int cur = sleep_head;
    while (cur >= 0){
        if (cur == pid){
            break;
        }
        prev = cur;
        cur = sleep_next[cur];
    }
    if (cur < 0){
        sleep_in_queue[pid] = 0;
        sleep_next[pid] = -1;
        return;
    }

    if (prev < 0){
        sleep_head = sleep_next[cur];
    } else{
        sleep_next[prev] = sleep_next[cur];
    }
    sleep_in_queue[cur] = 0;
    sleep_next[cur] = -1;
}

static void sleepq_insert_locked(int pid, unsigned long wake_tick){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }

    sleepq_remove_locked(pid);
    processes[pid].wake_tick = wake_tick;
    sleep_next[pid] = -1;
    sleep_in_queue[pid] = 1;

    if (sleep_head < 0){
        sleep_head = pid;
        return;
    }

    int prev = -1;
    int cur = sleep_head;
    while (cur >= 0){
        if ((long)(processes[cur].wake_tick - wake_tick) > 0){
            break;
        }
        prev = cur;
        cur = sleep_next[cur];
    }

    if (prev < 0){
        sleep_next[pid] = sleep_head;
        sleep_head = pid;
    } else{
        sleep_next[pid] = sleep_next[prev];
        sleep_next[prev] = pid;
    }
}

static int sleepq_pop_due_locked(unsigned long now_ticks){
    while (sleep_head >= 0){
        int pid = sleep_head;
        if (pid < 0 || pid >= MAX_PROCESSES){
            sleep_head = -1;
            return -1;
        }

        if (!sleep_in_queue[pid]){
            sleep_head = sleep_next[pid];
            sleep_next[pid] = -1;
            continue;
        }

        if (!tick_reached(now_ticks, processes[pid].wake_tick)){
            return -1;
        }

        sleep_head = sleep_next[pid];
        sleep_next[pid] = -1;
        sleep_in_queue[pid] = 0;
        return pid;
    }
    return -1;
}

static void wake_due_sleepers_locked(unsigned int local_core){
    while (1){
        int wake_pid = sleepq_pop_due_locked(system_ticks);
        if (wake_pid < 0){
            break;
        }
        if (wake_pid < 0 || wake_pid >= MAX_PROCESSES){
            continue;
        }
        if (processes[wake_pid].state != PROC_SLEEPING){
            continue;
        }
        processes[wake_pid].state = PROC_READY;
        processes[wake_pid].wake_tick = 0;
        unsigned int owner_core = processes[wake_pid].owner_core;
        if (owner_core >= MAX_CPU_CORES){
            owner_core = local_core;
            processes[wake_pid].owner_core = owner_core;
        }
        runq_enqueue(owner_core, wake_pid);
        mark_need_resched_locked(owner_core);
        if (owner_core != local_core){
            smp_send_ipi(owner_core);
        }
    }
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
    asm volatile("dmb ishst" : : : "memory");
    q->tail = (q->tail + 1) % MAX_PROCESSES;
    q->count++;
}

static unsigned int core_load_locked(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return 0;
    }
    unsigned int load = runq[core].count;
    int cur = current_pid[core];
    if (cur >= 0 && cur < MAX_PROCESSES && processes[cur].state == PROC_RUNNING){
        load++;
    }
    return load;
}

static unsigned int choose_least_loaded_core_locked(unsigned int preferred_core){
    if (preferred_core >= MAX_CPU_CORES){
        preferred_core = 0;
    }
    unsigned int best_core = preferred_core;
    unsigned int best_load = core_load_locked(preferred_core);
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        unsigned int load = core_load_locked(core);
        if (load < best_load){
            best_load = load;
            best_core = core;
        }
    }
    return best_core;
}

static int runq_dequeue_ready(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return -1;
    }

    run_queue_t* q = &runq[core];
    unsigned int checks = q->count;
    while (checks-- > 0 && q->count > 0){
        int pid = q->pid[q->head];
        asm volatile("dmb ish" : : : "memory");
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

static int runq_steal_ready(unsigned int thief_core){
    (void)thief_core;
    // Keep processes pinned to their owner core until cache coherency is proven.
    // Process creation still spreads work across cores, but migration can make
    // user stack/static buffers stale on hardware without confirmed SMPEN.
    return -1;
#if 0
    if (thief_core >= MAX_CPU_CORES){
        return -1;
    }

    unsigned int victim_core = MAX_CPU_CORES;
    unsigned int victim_load = 0;
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        if (core == thief_core){
            continue;
        }
        unsigned int load = runq[core].count;
        if (load > victim_load){
            victim_load = load;
            victim_core = core;
        }
    }
    if (victim_core >= MAX_CPU_CORES || victim_load == 0){
        return -1;
    }

    int pid = runq_dequeue_ready(victim_core);
    if (pid < 0){
        return -1;
    }
    processes[pid].owner_core = thief_core;
    return pid;
#endif
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

static int pid_running_on_other_core_locked(int pid, unsigned int core){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }
    for (unsigned int c = 0; c < MAX_CPU_CORES; c++){
        if (c == core){
            continue;
        }
        if (current_pid[c] == pid){
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
    sleep_in_queue[pid] = 0;
    sleep_next[pid] = -1;
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

static int process_create_common_locked(program_entry_t entry,
                                        int user_mode,
                                        void* user_sp,
                                        void* program_memory,
                                        unsigned long program_size,
                                        unsigned long user_rw_offset,
                                        unsigned long user_rw_size,
                                        int program_heap_alloc,
                                        unsigned int preferred_core){
    for (int i = 0; i < MAX_PROCESSES; i++){
        if (is_pid_pending_zombie(i)){
            continue;
        }
        if (processes[i].state != PROC_DEAD){
            continue;
        }

        void* stack = alloc_stack();
        if (!stack){
            return -1;
        }

        processes[i].entry = entry;
        processes[i].stack = stack;
        processes[i].state = PROC_READY;
        processes[i].wake_tick = 0;
        processes[i].program_memory = 0;
        processes[i].program_size = 0;
        processes[i].program_heap_alloc = 0;
        processes[i].user_mode = 0;
        processes[i].user_sp = 0;

        if (user_mode){
            processes[i].program_memory = program_memory;
            processes[i].program_size = program_size;
            processes[i].program_heap_alloc = program_heap_alloc;
            processes[i].user_mode = 1;
            processes[i].user_sp = user_sp;
            processes[i].sp = build_initial_context_el0(stack, (unsigned long)entry, user_sp);
            if (!program_memory ||
                mmu_process_space_create(i,
                                         (unsigned long)program_memory,
                                         program_size,
                                         user_rw_offset,
                                         user_rw_size) != 0){
                free_stack(stack);
                clear_process_descriptor(i);
                return -1;
            }
        } else{
            processes[i].sp = build_initial_context_el1(stack);
        }

        unsigned int owner_core = choose_least_loaded_core_locked(preferred_core);
        processes[i].owner_core = owner_core;
        for (int r = 0; r < 12; r++){
            processes[i].regs[r] = 0;
        }

        runq_enqueue(owner_core, i);
        if (owner_core != preferred_core){
            mark_need_resched_locked(owner_core);
            smp_send_ipi(owner_core);
        }
        return i;
    }
    return -1;
}

static void cleanup_init(process_cleanup_t* c){
    if (!c){
        return;
    }
    c->valid = 0;
    c->pid = -1;
    c->stack = 0;
    c->program_memory = 0;
    c->program_size = 0;
    c->program_heap_alloc = 0;
}

static void detach_process_resources_locked(int pid, process_cleanup_t* out){
    if (!out){
        return;
    }
    cleanup_init(out);
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }
    if (processes[pid].state == PROC_DEAD){
        return;
    }

    sleepq_remove_locked(pid);

    out->valid = 1;
    out->pid = pid;
    out->stack = processes[pid].stack;
    out->program_memory = processes[pid].program_memory;
    out->program_size = processes[pid].program_size;
    out->program_heap_alloc = processes[pid].program_heap_alloc;

    processes[pid].stack = 0;
    processes[pid].program_memory = 0;
    processes[pid].program_size = 0;
    processes[pid].program_heap_alloc = 0;
    processes[pid].sp = 0;
    processes[pid].state = PROC_REAPING;
}

static void release_process_resources(process_cleanup_t* c){
    if (!c || !c->valid){
        return;
    }
    int pid = c->pid;
    if (pid < 0 || pid >= MAX_PROCESSES){
        cleanup_init(c);
        return;
    }
    console_owner_on_process_exit(pid);
    socket_close_all_for_pid(pid);
    tls_session_close_all_for_pid(pid);
    mmu_process_space_destroy(pid);

    if (c->stack){
        free_stack(c->stack);
    }
    if (c->program_memory){
        if (c->program_heap_alloc){
            kfree_secure(c->program_memory, c->program_size);
        } else{
            volatile unsigned char* p = (volatile unsigned char*)c->program_memory;
            for (unsigned long i = 0; i < c->program_size; i++){
                p[i] = 0;
            }
            loader_free_program_memory(c->program_memory, c->program_size);
        }
    }

    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    clear_process_descriptor(pid);
    spin_unlock_irqrestore(&g_process_lock, irq);
    cleanup_init(c);
}

static void mark_current_for_reap(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return;
    }
    int pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        return;
    }
    if (processes[pid].state == PROC_DEAD || processes[pid].state == PROC_REAPING){
        return;
    }

    zombie_pid[core] = pid;
    processes[pid].state = PROC_REAPING;
    processes[pid].wake_tick = 0;
    sleepq_remove_locked(pid);
}

static void reap_pending_zombie_locked(unsigned int core, process_cleanup_t* cleanup){
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
    detach_process_resources_locked(pid, cleanup);
    zombie_pid[core] = -1;
}

static process_t* scheduler_next_for_core(unsigned int core){
    if (core >= MAX_CPU_CORES){
        return 0;
    }

    int next = -1;
    while (1){
        next = runq_dequeue_ready(core);
        if (next < 0){
            next = runq_steal_ready(core);
        }
        if (next < 0){
            return 0;
        }
        if (!pid_running_on_other_core_locked(next, core)){
            break;
        }
        double_run_blocked++;
        // This PID is still marked RUNNING on another core; skip it.
    }

    current_pid[core] = next;
    processes[next].state = PROC_RUNNING;
    return &processes[next];
}

static void mark_need_resched_locked(unsigned int core_id){
    if (core_id >= MAX_CPU_CORES){
        return;
    }
    need_resched[core_id] = 1;
    asm volatile("dmb ishst" : : : "memory");
}

void scheduler_tick(void){
    unsigned int core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    sched_ticks[core]++;
    wake_due_sleepers_locked(core);
    mark_need_resched_locked(core);
    spin_unlock_irqrestore(&g_process_lock, irq);
}

void scheduler_request_resched_core(unsigned int core_id){
    if (core_id >= MAX_CPU_CORES){
        return;
    }
    unsigned int local_core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    mark_need_resched_locked(core_id);
    spin_unlock_irqrestore(&g_process_lock, irq);
    if (core_id != local_core){
        smp_send_ipi(core_id);
    }
}

int scheduler_consume_need_resched(void){
    unsigned int core = scheduler_core_id();
    int pending = 0;
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    pending = (need_resched[core] != 0u) ? 1 : 0;
    need_resched[core] = 0;
    spin_unlock_irqrestore(&g_process_lock, irq);
    return pending;
}

void process_init(void){
    spinlock_init(&g_process_lock);
    mmu_process_spaces_reset();
    g_process_ready = 0;
    sleep_head = -1;
    for (int i = 0; i < MAX_PROCESSES; i++){
        clear_process_descriptor(i);
    }
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        current_pid[core] = -1;
        zombie_pid[core] = -1;
        need_resched[core] = 0;
        sched_ticks[core] = 0;
        runq_reset(core);
    }
    double_run_blocked = 0;
    g_process_ready = 1;
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
    unsigned int preferred_core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    int pid = process_create_common_locked(entry, 0, 0, 0, 0, 0, 0, 0, preferred_core);
    if (pid < 0){
        uart_puts("No stack available.\n");
    }
    spin_unlock_irqrestore(&g_process_lock, irq);
    return pid;
}

int process_create_loaded(loaded_program_t prog){
    void* user_sp = loader_user_stack_top(prog.memory);
    if (!user_sp){
        return -1;
    }

    unsigned int preferred_core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    int pid = process_create_common_locked(prog.entry,
                                           1,
                                           user_sp,
                                           prog.memory,
                                           prog.size,
                                           prog.user_rw_offset,
                                           prog.user_rw_size,
                                           prog.heap_allocated,
                                           preferred_core);
    spin_unlock_irqrestore(&g_process_lock, irq);
    return pid;
}

void process_exit(int pid){
    process_cleanup_t cleanup;
    cleanup_init(&cleanup);

    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    if (pid < 0 || pid >= MAX_PROCESSES){
        spin_unlock_irqrestore(&g_process_lock, irq);
        return;
    }
    if (processes[pid].state == PROC_DEAD || processes[pid].state == PROC_REAPING){
        spin_unlock_irqrestore(&g_process_lock, irq);
        return;
    }

    unsigned int owner_core = processes[pid].owner_core;
    if (owner_core >= MAX_CPU_CORES){
        owner_core = 0;
    }

    if (pid == current_pid[owner_core]){
        mark_current_for_reap(owner_core);
    } else{
        detach_process_resources_locked(pid, &cleanup);
    }
    spin_unlock_irqrestore(&g_process_lock, irq);
    release_process_resources(&cleanup);
}

void process_exit_current(void){
    unsigned int core = scheduler_core_id();
    int next_pid = -1;
    int next_user_mode = 0;
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    int pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        spin_unlock_irqrestore(&g_process_lock, irq);
        return;
    }

    mark_current_for_reap(core);
    // Keep current_pid pointing at this dead task until we actually switch
    // stacks. If there is no runnable task yet, this core idles on the current
    // kernel stack and it must not be reaped underneath us.

    process_t* next = scheduler_next_for_core(core);
    void* next_sp = next ? next->sp : 0;
    if (next){
        next_pid = current_pid[core];
        next_user_mode = next->user_mode;
    }
    spin_unlock_irqrestore(&g_process_lock, irq);
    if (next_sp){
        mmu_switch_to_pid(next_user_mode ? next_pid : -1);
        restore_context_and_eret(next_sp);
    }

    // No runnable task right now; park this core in recoverable idle.
    process_enter_idle_loop();
}

void process_fault_current(void){
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    mark_current_for_reap(scheduler_core_id());
    spin_unlock_irqrestore(&g_process_lock, irq);
}

process_t* get_process(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }
    return &processes[pid];
}

int process_current_pid(void){
    if (!g_process_ready){
        return -1;
    }
    return current_pid[scheduler_core_id()];
}

process_t* get_current_process(void){
    if (!g_process_ready){
        return 0;
    }
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
    process_cleanup_t cleanup;
    int next_pid = -1;
    int next_user_mode = 0;
    cleanup_init(&cleanup);

    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    reap_pending_zombie_locked(core, &cleanup);

    if (current_pid[core] >= 0 &&
        current_pid[core] < MAX_PROCESSES &&
        processes[current_pid[core]].state != PROC_DEAD &&
        processes[current_pid[core]].state != PROC_REAPING){
        spin_unlock_irqrestore(&g_process_lock, irq);
        release_process_resources(&cleanup);
        return;
    }

    process_t* next = scheduler_next_for_core(core);
    if (!next){
        spin_unlock_irqrestore(&g_process_lock, irq);
        release_process_resources(&cleanup);
        return;
    }
    void* next_sp = next->sp;
    next_pid = current_pid[core];
    next_user_mode = next->user_mode;
    spin_unlock_irqrestore(&g_process_lock, irq);
    release_process_resources(&cleanup);
    mmu_switch_to_pid(next_user_mode ? next_pid : -1);
    restore_context_and_eret(next_sp);
}

void process_yield(void){
    unsigned int core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    int pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        spin_unlock_irqrestore(&g_process_lock, irq);
        return;
    }

    processes[pid].state = PROC_READY;
    runq_enqueue(core, pid);
    spin_unlock_irqrestore(&g_process_lock, irq);
    asm volatile("wfi");
}

int scheduler_has_runnable(void){
    unsigned int core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    int pid = current_pid[core];
    if (pid >= 0 && pid < MAX_PROCESSES && processes[pid].state == PROC_RUNNING){
        spin_unlock_irqrestore(&g_process_lock, irq);
        return 1;
    }
    int r = runq_has_ready(core);
    spin_unlock_irqrestore(&g_process_lock, irq);
    return r;
}

void* scheduler_on_irq(void* irq_frame_sp){
    unsigned int core = scheduler_core_id();
    process_cleanup_t cleanup;
    int next_pid = -1;
    int next_user_mode = 0;
    cleanup_init(&cleanup);

    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    int cur = current_pid[core];

    reap_pending_zombie_locked(core, &cleanup);
    wake_due_sleepers_locked(core);

    if (cur >= 0 && cur < MAX_PROCESSES){
        if (processes[cur].state == PROC_RUNNING){
            processes[cur].state = PROC_READY;
            runq_enqueue(core, cur);
        }
        processes[cur].sp = irq_frame_sp;
    } else{
        process_t* next = scheduler_next_for_core(core);
        if (next){
            void* out_sp = next->sp;
            next_pid = current_pid[core];
            next_user_mode = next->user_mode;
            spin_unlock_irqrestore(&g_process_lock, irq);
            release_process_resources(&cleanup);
            mmu_switch_to_pid(next_user_mode ? next_pid : -1);
            return out_sp;
        }
        spin_unlock_irqrestore(&g_process_lock, irq);
        release_process_resources(&cleanup);
        mmu_switch_to_pid(-1);
        return irq_frame_sp;
    }

    process_t* next = scheduler_next_for_core(core);
    if (!next){
        int switched_to_idle = 0;
        if (cur >= 0 && cur < MAX_PROCESSES){
            if (processes[cur].state == PROC_SLEEPING){
                // Keep the process sleeping. Returning to the same frame lands
                // back in process_sleep()'s WFI loop until wake_tick is reached.
                spin_unlock_irqrestore(&g_process_lock, irq);
                release_process_resources(&cleanup);
                return irq_frame_sp;
            }
            if (processes[cur].state == PROC_DEAD || processes[cur].state == PROC_REAPING){
                // Still returning to the dead task's kernel frame, usually the
                // idle WFI loop in process_exit_current(). Keep ownership so
                // reap_pending_zombie() skips this stack until a real switch.
                current_pid[core] = cur;
            } else{
                processes[cur].state = PROC_RUNNING;
                current_pid[core] = cur;
            }
        } else{
            current_pid[core] = -1;
            switched_to_idle = 1;
        }
        spin_unlock_irqrestore(&g_process_lock, irq);
        release_process_resources(&cleanup);
        if (switched_to_idle){
            mmu_switch_to_pid(-1);
        }
        return irq_frame_sp;
    }

    void* out_sp = next->sp;
    next_pid = current_pid[core];
    next_user_mode = next->user_mode;
    spin_unlock_irqrestore(&g_process_lock, irq);
    release_process_resources(&cleanup);
    mmu_switch_to_pid(next_user_mode ? next_pid : -1);
    return out_sp;
}

void process_sleep(unsigned int ms){
    unsigned int core = scheduler_core_id();
    int pid = -1;
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    pid = current_pid[core];
    if (pid < 0 || pid >= MAX_PROCESSES){
        spin_unlock_irqrestore(&g_process_lock, irq);
        return;
    }

    sleepq_insert_locked(pid, system_ticks + ms);
    processes[pid].state = PROC_SLEEPING;
    spin_unlock_irqrestore(&g_process_lock, irq);

    // Ensure timer IRQ can wake this sleeper even when called from EL0 SVC path.
    unsigned long daif_prev;
    asm volatile("mrs %0, daif" : "=r"(daif_prev));
    asm volatile("msr daifclr, #2" : : : "memory");

    // Block cooperatively until the timer IRQ path wakes us.
    while (1){
        irq = spin_lock_irqsave(&g_process_lock);
        process_state_t st = processes[pid].state;
        if (st != PROC_SLEEPING){
            // If a remote core woke us, we may still be running on this core's
            // stack; normalize state back to RUNNING before returning to EL0.
            if (st == PROC_READY &&
                core < MAX_CPU_CORES &&
                current_pid[core] == pid){
                processes[pid].state = PROC_RUNNING;
            }
            spin_unlock_irqrestore(&g_process_lock, irq);
            break;
        }
        spin_unlock_irqrestore(&g_process_lock, irq);
        asm volatile("wfi");
    }

    asm volatile("msr daif, %0" : : "r"(daif_prev) : "memory");
}

void process_dump(void){
    unsigned long irq = spin_lock_irqsave(&g_process_lock);
    uart_puts("PID STATE CORE WAKE\n");
    for (int i = 0; i < MAX_PROCESSES; i++){
        uart_send('0' + i);
        uart_puts(" ");
        switch (processes[i].state){
            case PROC_DEAD: uart_puts("DEAD "); break;
            case PROC_READY: uart_puts("READY "); break;
            case PROC_RUNNING: uart_puts("RUN "); break;
            case PROC_SLEEPING: uart_puts("SLEEP "); break;
            case PROC_REAPING: uart_puts("REAP "); break;
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
    uart_puts("ONLINE_MASK=");
    uart_puthex(smp_online_mask());
    uart_puts("\n");
    uart_puts("SCHED_TICKS ");
    for (unsigned int core = 0; core < MAX_CPU_CORES; core++){
        uart_send((char)('0' + (core & 0xF)));
        uart_puts("=");
        uart_puthex((unsigned int)sched_ticks[core]);
        uart_puts(" ");
    }
    uart_puts("\n");
    uart_puts("DOUBLE_RUN_BLOCKED=");
    uart_puthex((unsigned int)double_run_blocked);
    uart_puts("\n");
    spin_unlock_irqrestore(&g_process_lock, irq);
}

__attribute__((noreturn)) void process_enter_idle_loop(void){
    unsigned int core = scheduler_core_id();
    unsigned long irq = spin_lock_irqsave(&g_process_lock);

    if (core < MAX_CPU_CORES){
        int pid = current_pid[core];
        if (pid >= 0 && pid < MAX_PROCESSES){
            if (processes[pid].state != PROC_DEAD && processes[pid].state != PROC_REAPING){
                mark_current_for_reap(core);
            }
        }
    }

    spin_unlock_irqrestore(&g_process_lock, irq);
    mmu_switch_to_pid(-1);

    asm volatile("msr daifclr, #2" : : : "memory");
    while (1){
        if (scheduler_has_runnable()){
            scheduler_run_once();
        } else{
            asm volatile("wfi");
        }
    }
}
