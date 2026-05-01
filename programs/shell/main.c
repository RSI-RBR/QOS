#include "syscall.h"

#define BUF_SIZE 128

static char g_buf[BUF_SIZE];
static int g_len = 0;

static int str_eq(const char* a, const char* b){
    while (*a && *b){
        if (*a != *b){
            return 0;
        }
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static void print_prompt(void){
    qos_puts("\nUQOS> ");
}

static void print_uint(unsigned int v){
    char tmp[16];
    int n = 0;
    if (v == 0){
        qos_putc('0');
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0){
        qos_putc(tmp[--n]);
    }
}

static void cmd_help(void){
    qos_puts("Commands:\n");
    qos_puts(" help\n");
    qos_puts(" run\n");
    qos_puts(" clear\n");
    qos_puts(" fbinfo\n");
}

static void cmd_run(void){
    int pid = qos_run_program();
    if (pid < 0){
        qos_puts("Program load failed.\n");
        return;
    }
    qos_puts("Program queued as PID ");
    print_uint((unsigned int)pid);
    qos_puts("\n");
}

static void cmd_fbinfo(void){
    unsigned int w = qos_get_screen_width();
    unsigned int h = qos_get_screen_height();
    qos_puts("FB width=");
    print_uint(w);
    qos_puts(" height=");
    print_uint(h);
    qos_puts("\n");
}

static void execute_line(void){
    if (g_len <= 0){
        return;
    }
    g_buf[g_len] = 0;

    if (str_eq(g_buf, "help")){
        cmd_help();
    } else if (str_eq(g_buf, "run")){
        cmd_run();
    } else if (str_eq(g_buf, "clear")){
        qos_fb_clear(0x00000000);
    } else if (str_eq(g_buf, "fbinfo")){
        cmd_fbinfo();
    } else{
        qos_puts("Unknown command.\n");
    }
}

void program_main(void){
    qos_puts("User shell ready.");
    print_prompt();

    while (1){
        int ch = qos_try_getc();
        if (ch < 0){
            // Keep shell RUNNING; timer IRQ preemption will schedule peers.
            // This avoids sleep edge-cases when no alternate runnable task exists.
            continue;
        }

        if (ch == '\r' || ch == '\n'){
            qos_puts("\n");
            execute_line();
            g_len = 0;
            g_buf[0] = 0;
            print_prompt();
            continue;
        }

        if (ch == 127 || ch == '\b'){
            if (g_len > 0){
                g_len--;
                g_buf[g_len] = 0;
                qos_puts("\b \b");
            }
            continue;
        }

        if (g_len < (BUF_SIZE - 1)){
            g_buf[g_len++] = (char)ch;
            qos_putc((char)ch);
        }
    }
}
