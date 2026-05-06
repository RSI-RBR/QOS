#include "console.h"
#include "uart.h"
#include "process.h"
#include "spinlock.h"

static volatile int g_console_owner_pid = -1;
static volatile int g_console_prev_owner_pid = -1;
static spinlock_t g_console_lock;

static int process_is_alive_locked(int pid);

static int console_pick_recovery_owner_locked(void){
    // Prefer PID 0 (boot user shell in this system) when alive.
    if (process_is_alive_locked(0)){
        return 0;
    }
    // Fallback: first alive process.
    for (int pid = 1; pid < MAX_PROCESSES; pid++){
        if (process_is_alive_locked(pid)){
            return pid;
        }
    }
    return -1;
}

static int process_is_alive_locked(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }
    process_t* p = get_process(pid);
    if (!p){
        return 0;
    }
    return p->state != PROC_DEAD && p->state != PROC_REAPING;
}

void console_init(void){
    spinlock_init(&g_console_lock);
    // Strict foreground ownership: no implicit owner until claimed.
    g_console_owner_pid = -1;
    g_console_prev_owner_pid = -1;
}

int console_try_getc_for_pid(int pid, char* out){
    if (!out || pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    int owner = g_console_owner_pid;
    if (owner >= 0 && !process_is_alive_locked(owner)){
        g_console_owner_pid = -1;
        owner = -1;
    }
    // Recovery mode: when no owner is present, allow the active requester
    // (typically userspace shell) to auto-claim input ownership.
    if (owner < 0 && process_is_alive_locked(pid)){
        g_console_owner_pid = pid;
        owner = pid;
    } else if (owner < 0){
        int recover = console_pick_recovery_owner_locked();
        if (recover >= 0){
            g_console_owner_pid = recover;
            owner = recover;
        }
    }
    spin_unlock_irqrestore(&g_console_lock, irq);
    if (owner < 0 || owner != pid){
        return 0;
    }
    return uart_try_getc(out);
}

int console_set_owner(int requester_pid, int target_pid){
    if (requester_pid < 0 || requester_pid >= MAX_PROCESSES){
        return -1;
    }
    if (target_pid < 0 || target_pid >= MAX_PROCESSES){
        return -1;
    }
    if (!process_is_alive_locked(target_pid)){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    int owner = g_console_owner_pid;
    if (owner >= 0 && !process_is_alive_locked(owner)){
        g_console_owner_pid = -1;
        g_console_prev_owner_pid = -1;
        owner = -1;
    }
    if (owner >= 0 && owner != requester_pid){
        spin_unlock_irqrestore(&g_console_lock, irq);
        return -1;
    }
    // Allow initial claim from an unowned state only by self-claim.
    if (owner < 0 && requester_pid != target_pid){
        spin_unlock_irqrestore(&g_console_lock, irq);
        return -1;
    }

    if (target_pid != requester_pid){
        // Explicit handoff (shell -> child). Track who gave up foreground.
        g_console_prev_owner_pid = requester_pid;
    } else if (owner != requester_pid){
        // Fresh self-claim from unowned/auto-recovery state.
        g_console_prev_owner_pid = -1;
    }
    g_console_owner_pid = target_pid;
    spin_unlock_irqrestore(&g_console_lock, irq);
    return 0;
}

int console_release_owner(int requester_pid){
    if (requester_pid < 0 || requester_pid >= MAX_PROCESSES){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    if (g_console_owner_pid == requester_pid){
        g_console_owner_pid = -1;
        g_console_prev_owner_pid = -1;
        spin_unlock_irqrestore(&g_console_lock, irq);
        return 0;
    }
    spin_unlock_irqrestore(&g_console_lock, irq);
    return -1;
}

int console_get_owner(void){
    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    int owner = g_console_owner_pid;
    if (owner >= 0 && !process_is_alive_locked(owner)){
        g_console_owner_pid = -1;
        g_console_prev_owner_pid = -1;
        owner = -1;
    }
    spin_unlock_irqrestore(&g_console_lock, irq);
    return owner;
}

void console_owner_on_process_exit(int pid){
    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    if (g_console_owner_pid != pid){
        spin_unlock_irqrestore(&g_console_lock, irq);
        return;
    }

    int restore = g_console_prev_owner_pid;
    if (restore >= 0 && restore < MAX_PROCESSES && process_is_alive_locked(restore)){
        g_console_owner_pid = restore;
    } else{
        int recover = console_pick_recovery_owner_locked();
        g_console_owner_pid = recover;
    }
    g_console_prev_owner_pid = -1;
    spin_unlock_irqrestore(&g_console_lock, irq);
}
