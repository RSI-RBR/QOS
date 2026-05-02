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
    qos_puts(" usbstat\n");
    qos_puts(" netstat\n");
    qos_puts(" netloop\n");
    qos_puts(" netpoll\n");
    qos_puts(" ping\n");
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

static void cmd_usbstat(void){
    qos_usb_dump_info();
}

static void cmd_netstat(void){
    qos_net_dump_stats();
}

static void cmd_netpoll(void){
    int delivered = qos_net_poll();
    qos_puts("NET poll delivered=");
    print_uint((unsigned int)(delivered < 0 ? 0 : delivered));
    qos_puts("\n");
}

static void cmd_netloop(void){
    static unsigned char rx[1536];
    int rc = qos_net_send_test_frame();
    if (rc != 0){
        qos_puts("NET loop send failed.\n");
        return;
    }
    (void)qos_net_poll();
    int n = qos_net_recv_raw(rx, sizeof(rx));
    if (n <= 0){
        qos_puts("NET loop recv empty.\n");
        return;
    }
    qos_puts("NET loop recv bytes=");
    print_uint((unsigned int)n);
    qos_puts("\n");
}

static void cmd_ping(void){
    int rtt = qos_net_ping_gateway(1000);
    if (rtt >= 0){
        qos_puts("PING reply time=");
        print_uint((unsigned int)rtt);
        qos_puts(" ms\n");
        return;
    }
    if (rtt == -1){
        qos_puts("PING timeout\n");
    } else if (rtt == -2){
        qos_puts("PING blocked: gateway MAC unresolved\n");
    } else{
        qos_puts("PING send failed\n");
    }
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
    } else if (str_eq(g_buf, "usbstat")){
        cmd_usbstat();
    } else if (str_eq(g_buf, "netstat")){
        cmd_netstat();
    } else if (str_eq(g_buf, "netloop")){
        cmd_netloop();
    } else if (str_eq(g_buf, "netpoll")){
        cmd_netpoll();
    } else if (str_eq(g_buf, "ping")){
        cmd_ping();
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
