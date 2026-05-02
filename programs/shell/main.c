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

static unsigned short read_be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static int dns_skip_name(const unsigned char* msg, int len, int off){
    if (!msg || len <= 0 || off < 0 || off >= len){
        return -1;
    }
    while (off < len){
        unsigned char c = msg[off];
        if (c == 0){
            return off + 1;
        }
        if ((c & 0xC0u) == 0xC0u){
            if (off + 1 >= len){
                return -1;
            }
            return off + 2;
        }
        if (c > 63u){
            return -1;
        }
        off++;
        if (off + (int)c > len){
            return -1;
        }
        off += (int)c;
    }
    return -1;
}

static int dns_build_qname(const char* host, unsigned char* out, int cap){
    int w = 0;
    int label_len = 0;
    int label_pos = -1;
    const char* p = host;
    if (!host || !out || cap <= 0){
        return -1;
    }

    label_pos = w++;
    if (w >= cap){
        return -1;
    }

    while (*p){
        char ch = *p++;
        if (ch == '.'){
            if (label_len <= 0 || label_len > 63){
                return -1;
            }
            out[label_pos] = (unsigned char)label_len;
            label_len = 0;
            label_pos = w++;
            if (w >= cap){
                return -1;
            }
            continue;
        }
        if (label_len >= 63){
            return -1;
        }
        if (w >= cap){
            return -1;
        }
        out[w++] = (unsigned char)ch;
        label_len++;
    }
    if (label_len <= 0 || label_len > 63){
        return -1;
    }
    out[label_pos] = (unsigned char)label_len;
    if (w >= cap){
        return -1;
    }
    out[w++] = 0;
    return w;
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
    qos_puts(" dnscheck <domain>\n");
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
    static const unsigned char dns_server[4] = {10, 0, 0, 1};
    static unsigned short dns_id = 0x5153u;
    unsigned char q[320];
    unsigned char r[600];
    unsigned int qi = 0;
    int qname_len;
    int rc;
    udp_meta_t meta;
    int tries;
    int got_answer = 0;

    if (!host || !*host){
        qos_puts("Usage: dnscheck <domain>\n");
        return;
    }

    qname_len = dns_build_qname(host, &q[12], (int)(sizeof(q) - 16));
    if (qname_len <= 0){
        qos_puts("Invalid domain format.\n");
        return;
    }

    // DNS header
    q[qi++] = (unsigned char)(dns_id >> 8);
    q[qi++] = (unsigned char)(dns_id & 0xFFu);
    q[qi++] = 0x01; q[qi++] = 0x00; // RD=1
    q[qi++] = 0x00; q[qi++] = 0x01; // QDCOUNT=1
    q[qi++] = 0x00; q[qi++] = 0x00; // ANCOUNT=0
    q[qi++] = 0x00; q[qi++] = 0x00; // NSCOUNT=0
    q[qi++] = 0x00; q[qi++] = 0x00; // ARCOUNT=0
    qi += (unsigned int)qname_len;
    q[qi++] = 0x00; q[qi++] = 0x01; // QTYPE=A
    q[qi++] = 0x00; q[qi++] = 0x01; // QCLASS=IN

    rc = qos_net_udp_send(dns_server, 4053u, 53u, q, qi);
    if (rc != 0){
        qos_puts("DNS query send failed.\n");
        return;
    }

    qos_puts("Resolving ");
    qos_puts(host);
    qos_puts("...\n");

    for (tries = 0; tries < 300; tries++){ // ~3s at 10ms step
        (void)qos_net_poll();

        while (1){
            int n = qos_net_udp_recv(&meta, r, sizeof(r));
            if (n <= 0){
                break;
            }
            if (meta.src_port != 53u){
                continue;
            }
            if (n < 12){
                continue;
            }
            if (read_be16(&r[0]) != dns_id){
                continue;
            }

            unsigned short flags = read_be16(&r[2]);
            unsigned short qdcount = read_be16(&r[4]);
            unsigned short ancount = read_be16(&r[6]);
            unsigned int rcode = (unsigned int)(flags & 0x000Fu);
            int off = 12;
            unsigned int ai;
            int printed = 0;

            if (rcode != 0u){
                qos_puts("DNS error rcode=");
                print_uint(rcode);
                qos_puts("\n");
                dns_id++;
                return;
            }

            for (ai = 0; ai < qdcount; ai++){
                off = dns_skip_name(r, n, off);
                if (off < 0 || off + 4 > n){
                    dns_id++;
                    qos_puts("DNS malformed response.\n");
                    return;
                }
                off += 4; // qtype + qclass
            }

            for (ai = 0; ai < ancount; ai++){
                unsigned short type;
                unsigned short classv;
                unsigned short rdlen;
                off = dns_skip_name(r, n, off);
                if (off < 0 || off + 10 > n){
                    break;
                }
                type = read_be16(&r[off + 0]);
                classv = read_be16(&r[off + 2]);
                rdlen = read_be16(&r[off + 8]);
                off += 10;
                if (off + rdlen > n){
                    break;
                }
                if (type == 1u && classv == 1u && rdlen == 4u){
                    qos_puts("A ");
                    print_ip4(&r[off]);
                    qos_puts("\n");
                    printed = 1;
                    got_answer = 1;
                }
                off += rdlen;
            }

            if (!printed){
                qos_puts("No A record in response.\n");
                got_answer = 1;
            }
            dns_id++;
            return;
        }

        qos_sleep(10);
    }

    if (!got_answer){
        qos_puts("DNS timeout.\n");
        dns_id++;
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
    } else if (str_starts_with(g_buf, "dnscheck ")){
        const char* host = g_buf + 9;
        while (*host == ' '){
            host++;
        }
        cmd_dnscheck(host);
    } else if (str_eq(g_buf, "dnscheck")){
        qos_puts("Usage: dnscheck <domain>\n");
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
