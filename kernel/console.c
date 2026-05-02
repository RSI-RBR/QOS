#include "console.h"
#include "uart.h"
#include "process.h"

static volatile int g_console_owner_pid = -1;

void console_init(void){
    g_console_owner_pid = -1;
}

int console_try_getc_for_pid(int pid, char* out){
    if (g_console_owner_pid >= 0 && g_console_owner_pid != pid){
        return 0;
    }
    return uart_try_getc(out);
}

int console_set_owner(int requester_pid, int target_pid){
    if (target_pid < -1 || target_pid >= MAX_PROCESSES){
        return -1;
    }

    if (g_console_owner_pid >= 0 && g_console_owner_pid != requester_pid){
        return -1;
    }

    g_console_owner_pid = target_pid;
    return 0;
}

int console_release_owner(int requester_pid){
    if (g_console_owner_pid < 0 || g_console_owner_pid == requester_pid){
        g_console_owner_pid = -1;
        return 0;
    }
    return -1;
}

int console_get_owner(void){
    return g_console_owner_pid;
}

void console_owner_on_process_exit(int pid){
    if (g_console_owner_pid == pid){
        process_t* shell = get_process(0);
        if (pid != 0 && shell && shell->state != PROC_DEAD){
            g_console_owner_pid = 0;
        } else{
            g_console_owner_pid = -1;
        }
    }
}
