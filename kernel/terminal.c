#include "terminal.h"
#include "fb_console.h"
#include "uart.h"
#include "console.h"
#include "remote_login.h"
#include "spinlock.h"

#define TERM_FLAG_UART 1u
#define TERM_FLAG_FB   2u

typedef struct {
    int id;
    int foreground_pid;
    unsigned int flags;
} terminal_t;

static terminal_t g_terms[QOS_TERMINAL_MAX];
static int g_active_term = 0;
static spinlock_t g_terminal_lock;

static int terminal_should_mirror_to_fb_locked(int pid, int owner){
    if (g_active_term < 0 || g_active_term >= QOS_TERMINAL_MAX){
        return 0;
    }
    if ((g_terms[g_active_term].flags & TERM_FLAG_FB) == 0u){
        return 0;
    }
    if (pid < 0){
        return 1;
    }
    return owner == pid;
}

void terminal_init(void){
    spinlock_init(&g_terminal_lock);
    for (int i = 0; i < QOS_TERMINAL_MAX; i++){
        g_terms[i].id = i;
        g_terms[i].foreground_pid = -1;
        g_terms[i].flags = TERM_FLAG_UART;
    }
    g_active_term = 0;
    g_terms[0].flags = TERM_FLAG_UART | TERM_FLAG_FB;
    fb_console_init();
}

void terminal_clear_active(void){
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    if (g_active_term >= 0 &&
        g_active_term < QOS_TERMINAL_MAX &&
        (g_terms[g_active_term].flags & TERM_FLAG_FB)){
        fb_console_clear();
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

void terminal_putc_for_pid(int pid, char c){
    int owner = (pid >= 0) ? console_get_owner() : -1;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int mirror_fb = terminal_should_mirror_to_fb_locked(pid, owner);
    int mirror_remote = (pid >= 0 && owner == pid);

    if (mirror_remote){
        remote_login_on_tty_output_char(c);
    }
    if (c == '\n'){
        uart_send('\r');
    }
    uart_send(c);
    if (mirror_fb){
        fb_console_putc(c);
    }

    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

void terminal_write_for_pid(int pid, const char* s, unsigned long len){
    if (!s){
        return;
    }

    int owner = (pid >= 0) ? console_get_owner() : -1;
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int mirror_fb = terminal_should_mirror_to_fb_locked(pid, owner);
    int mirror_remote = (pid >= 0 && owner == pid);

    for (unsigned long i = 0; i < len; i++){
        char c = s[i];
        if (mirror_remote){
            remote_login_on_tty_output_char(c);
        }
        if (c == '\n'){
            uart_send('\r');
        }
        uart_send(c);
    }
    if (mirror_fb){
        fb_console_write(s, len);
    }

    spin_unlock_irqrestore(&g_terminal_lock, irq);
}

int terminal_try_getc_for_pid(int pid, char* out){
    return console_try_getc_for_pid(pid, out);
}

int terminal_try_getc_for_pid_ex(int pid, char* out, unsigned int* out_source){
    return console_try_getc_for_pid_ex(pid, out, out_source);
}

int terminal_get_active(void){
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    int active = g_active_term;
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return active;
}

int terminal_set_active(int id){
    if (id < 0 || id >= QOS_TERMINAL_MAX){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_terminal_lock);
    g_active_term = id;
    if (g_terms[g_active_term].flags & TERM_FLAG_FB){
        fb_console_clear();
    }
    spin_unlock_irqrestore(&g_terminal_lock, irq);
    return 0;
}
