#include "syscall.h"
#include "user_net.h"

#define BUF_SIZE 128

static char g_buf[BUF_SIZE];
static int g_len = 0;
static int g_tty_owned = 1;
static int g_shell_pid = 0;

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

static int str_starts_with(const char* s, const char* prefix){
    while (*prefix){
        if (*s != *prefix){
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
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

static void print_ip4(const unsigned char ip[4]){
    print_uint((unsigned int)ip[0]);
    qos_putc('.');
    print_uint((unsigned int)ip[1]);
    qos_putc('.');
    print_uint((unsigned int)ip[2]);
    qos_putc('.');
    print_uint((unsigned int)ip[3]);
}

static const unsigned char g_dns_server[4] = {10, 0, 0, 1};

static int dns_resolve_a(const char* host, unsigned char out_ip[4], int verbose){
    int rc = qos_dns_resolve_a_socket(host, g_dns_server, out_ip, 3000u);
    if (rc == 0){
        if (verbose){
            qos_puts("A ");
            print_ip4(out_ip);
            qos_puts("\n");
        }
        return 0;
    }
    if (verbose){
        if (rc == QOS_DNS_ERR_TIMEOUT){
            qos_puts("DNS timeout.\n");
        } else{
            qos_puts("DNS resolve failed.\n");
        }
    }
    return -1;
}

static void cmd_help(void){
    qos_puts("Commands:\n");
    qos_puts(" help\n");
    qos_puts(" run\n");
    qos_puts(" web\n");
    qos_puts(" ps\n");
    qos_puts(" validate\n");
    qos_puts(" clear\n");
    qos_puts(" fbinfo\n");
    qos_puts(" usbstat\n");
    qos_puts(" netstat\n");
    qos_puts(" ping\n");
    qos_puts(" dnscheck <domain>\n");
    qos_puts(" httpget <host> [path]\n");
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

static void cmd_web(void){
    // FAT 8.3 uppercase, space-padded: "WEBBROWSBIN"
    static const char web_file_83[] = "WEBBROWSBIN";
    int pid = qos_run_program_named(web_file_83);
    if (pid < 0){
        qos_puts("WEBBROWS.BIN load failed.\n");
        return;
    }
    if (qos_tty_set_owner(pid) != 0){
        qos_puts("Warning: could not transfer TTY ownership.\n");
    } else{
        g_tty_owned = 0;
    }
    qos_puts("WebBrowser queued as PID ");
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

static void cmd_ps(void){
    qos_process_dump();
}

static void cmd_ping(void){
    int rtt = qos_net_ping_gateway(2000);
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

static void cmd_dnscheck(const char* host){
    unsigned char ip[4];
    if (!host || !*host){
        qos_puts("Usage: dnscheck <domain>\n");
        return;
    }
    if (dns_resolve_a(host, ip, 1) != 0){
        qos_puts("DNS resolve failed.\n");
    }
}

static void cmd_httpget(const char* host, const char* path){
    static unsigned char resp[8192];
    unsigned char ip[4];
    const char* req_path = (path && *path) ? path : "/";
    int n;

    if (!host || !*host){
        qos_puts("Usage: httpget <host> [path]\n");
        return;
    }

    if (dns_resolve_a(host, ip, 0) != 0){
        qos_puts("DNS resolve failed.\n");
        return;
    }

    qos_puts("Connecting to ");
    qos_puts(host);
    qos_puts(" (");
    print_ip4(ip);
    qos_puts(") path ");
    qos_puts(req_path);
    qos_puts("\n");

    n = qos_net_tcp_http_get(ip, host, req_path, resp, sizeof(resp));
    if (n < 0){
        qos_puts("HTTP GET failed.\n");
        return;
    }

    qos_puts("HTTP bytes=");
    print_uint((unsigned int)n);
    qos_puts("\n");
    for (int i = 0; i < n; i++){
        unsigned char c = resp[i];
        if (c == '\r' || c == '\n' || (c >= 32u && c <= 126u)){
            qos_putc((char)c);
        } else{
            qos_putc('.');
        }
    }
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
    } else if (str_eq(g_buf, "web")){
        cmd_web();
    } else if (str_eq(g_buf, "clear")){
        qos_fb_clear(0x00000000);
    } else if (str_eq(g_buf, "fbinfo")){
        cmd_fbinfo();
    } else if (str_eq(g_buf, "usbstat")){
        cmd_usbstat();
    } else if (str_eq(g_buf, "netstat")){
        cmd_netstat();
    } else if (str_eq(g_buf, "ps")){
        cmd_ps();
    } else if (str_eq(g_buf, "validate")){
        cmd_ps();
    } else if (str_eq(g_buf, "ping")){
        cmd_ping();
    } else if (str_starts_with(g_buf, "dnscheck ")){
        const char* host = g_buf + 9;
        while (*host == ' '){
            host++;
        }
        cmd_dnscheck(host);
    } else if (str_eq(g_buf, "dnscheck")){
        qos_puts("Usage: dnscheck <domain>\n");
    } else if (str_starts_with(g_buf, "httpget ")){
        char* p = g_buf + 8;
        char* host;
        char* path = 0;
        while (*p == ' '){
            p++;
        }
        host = p;
        while (*p && *p != ' '){
            p++;
        }
        if (*p){
            *p++ = 0;
            while (*p == ' '){
                p++;
            }
            if (*p){
                path = p;
            }
        }
        if (!*host){
            qos_puts("Usage: httpget <host> [path]\n");
        } else{
            cmd_httpget(host, path);
        }
    } else if (str_eq(g_buf, "httpget")){
        qos_puts("Usage: httpget <host> [path]\n");
    } else{
        qos_puts("Unknown command.\n");
    }
}

void program_main(void){
    g_shell_pid = qos_getpid();
    if (g_shell_pid < 0){
        g_shell_pid = 0;
    }
    // Claim foreground console ownership explicitly on startup.
    (void)qos_tty_set_owner(g_shell_pid);
    g_tty_owned = 1;

    qos_puts("User shell ready.");
    print_prompt();

    while (1){
        int owner = qos_tty_get_owner();
        int shell_has_tty = (owner == g_shell_pid);
        // Recover ownership when console is unowned or owned by a dead/exited task.
        if (!shell_has_tty){
            if (qos_tty_set_owner(g_shell_pid) == 0){
                owner = g_shell_pid;
                shell_has_tty = 1;
            }
        }
        if (shell_has_tty && !g_tty_owned){
            g_tty_owned = 1;
            qos_puts("\nReturned to shell.");
            print_prompt();
        } else if (!shell_has_tty){
            g_tty_owned = 0;
            continue;
        }

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
