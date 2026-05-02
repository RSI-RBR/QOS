#include "console.h"
#include "uart.h"
#include "process.h"
#include "spinlock.h"

static volatile int g_console_owner_pid = -1;
static spinlock_t g_console_lock;

static int process_is_alive_locked(int pid){
    if (pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }
    process_t* p = get_process(pid);
    if (!p){
        return 0;
    }
    return p->state != PROC_DEAD;
}

void console_init(void){
    spinlock_init(&g_console_lock);
    // Strict foreground ownership: no implicit owner until claimed.
    g_console_owner_pid = -1;
}

int console_try_getc_for_pid(int pid, char* out){
    if (!out || pid < 0 || pid >= MAX_PROCESSES){
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    int owner = g_console_owner_pid;
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

    g_console_owner_pid = target_pid;
    spin_unlock_irqrestore(&g_console_lock, irq);
    uart_puts("DBG TTY set req=");
    uart_putdec((unsigned long)requester_pid);
    uart_puts(" -> ");
    uart_putdec((unsigned long)target_pid);
    uart_puts("\n");
    return 0;
}

int console_release_owner(int requester_pid){
    if (requester_pid < 0 || requester_pid >= MAX_PROCESSES){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    if (g_console_owner_pid == requester_pid){
        g_console_owner_pid = -1;
        spin_unlock_irqrestore(&g_console_lock, irq);
        uart_puts("DBG TTY release req=");
        uart_putdec((unsigned long)requester_pid);
        uart_puts("\n");
        return 0;
    }
    spin_unlock_irqrestore(&g_console_lock, irq);
    return -1;
}

int console_get_owner(void){
    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    int owner = g_console_owner_pid;
    spin_unlock_irqrestore(&g_console_lock, irq);
    return owner;
}

void console_owner_on_process_exit(int pid){
    unsigned long irq = spin_lock_irqsave(&g_console_lock);
    if (g_console_owner_pid != pid){
        spin_unlock_irqrestore(&g_console_lock, irq);
        return;
    }

    // Do not hardcode a shell PID here; userspace shell reclaims ownership.
    g_console_owner_pid = -1;
    spin_unlock_irqrestore(&g_console_lock, irq);
}
