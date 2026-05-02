#include "syscall.h"
#include "user_net.h"

#define INPUT_CAP 192
#define REQ_CAP 512
#define RESP_CAP 16384
#define DNS_TIMEOUT_MS 3000u
#define RECV_TIMEOUT_MS 6000u

static char g_input[INPUT_CAP];
static int g_input_len = 0;

static const unsigned char g_dns_server[4] = {10, 0, 0, 1};

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

static int append_char(char* dst, int cap, int* idx, char c){
    if (*idx >= cap - 1){
        return -1;
    }
    dst[*idx] = c;
    (*idx)++;
    dst[*idx] = 0;
    return 0;
}

static int append_str(char* dst, int cap, int* idx, const char* s){
    while (*s){
        if (append_char(dst, cap, idx, *s++) != 0){
            return -1;
        }
    }
    return 0;
}

static int find_http_body(const unsigned char* resp, int len){
    for (int i = 0; i + 3 < len; i++){
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n'){
            return i + 4;
        }
    }
    return 0;
}

static int match_entity(const unsigned char* p, int rem, const char* ent){
    int i = 0;
    while (ent[i]){
        if (i >= rem || (unsigned char)ent[i] != p[i]){
            return 0;
        }
        i++;
    }
    return i;
}

static unsigned char ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int tag_name_is(const unsigned char* s, int len, const char* name){
    int i = 0;
    while (name[i]){
        if (i >= len){
            return 0;
        }
        if (ascii_lower(s[i]) != (unsigned char)name[i]){
            return 0;
        }
        i++;
    }
    if (i < len){
        unsigned char c = s[i];
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>')){
            return 0;
        }
    }
    return 1;
}

static void print_html_text(const unsigned char* html, int len){
    int in_tag = 0;
    int last_space = 1;
    int suppress_style = 0;
    int suppress_script = 0;
    int suppress_head = 0;
    int tag_start = -1;

    for (int i = 0; i < len; i++){
        unsigned char c = html[i];

        if (in_tag){
            if (c == '>'){
                int ts = tag_start >= 0 ? tag_start : i;
                int te = i;
                while (ts < te && (html[ts] == ' ' || html[ts] == '\t' || html[ts] == '\r' || html[ts] == '\n')){
                    ts++;
                }
                int closing = 0;
                if (ts < te && html[ts] == '/'){
                    closing = 1;
                    ts++;
                }
                while (ts < te && (html[ts] == ' ' || html[ts] == '\t' || html[ts] == '\r' || html[ts] == '\n')){
                    ts++;
                }
                int nlen = te - ts;
                if (nlen > 0){
                    if (tag_name_is(&html[ts], nlen, "style")){
                        suppress_style = closing ? 0 : 1;
                    } else if (tag_name_is(&html[ts], nlen, "script")){
                        suppress_script = closing ? 0 : 1;
                    } else if (tag_name_is(&html[ts], nlen, "head")){
                        suppress_head = closing ? 0 : 1;
                    }
                }
                in_tag = 0;
                tag_start = -1;
                if (!last_space){
                    qos_putc(' ');
                    last_space = 1;
                }
            }
            continue;
        }

        if (c == '<'){
            in_tag = 1;
            tag_start = i + 1;
            continue;
        }

        if (suppress_style || suppress_script || suppress_head){
            continue;
        }

        if (c == '&'){
            int rem = len - i;
            int m = 0;
            if ((m = match_entity(&html[i], rem, "&nbsp;")) > 0 ||
                (m = match_entity(&html[i], rem, "&#160;")) > 0){
                c = ' ';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&amp;")) > 0){
                c = '&';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&lt;")) > 0){
                c = '<';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&gt;")) > 0){
                c = '>';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&quot;")) > 0){
                c = '"';
                i += (m - 1);
            } else if ((m = match_entity(&html[i], rem, "&#39;")) > 0){
                c = '\'';
                i += (m - 1);
            }
        }

        if (c == '\r'){
            continue;
        }

        if (c == '\n' || c == '\t' || c == ' '){
            if (!last_space){
                qos_putc(' ');
                last_space = 1;
            }
            continue;
        }

        if (c < 32u || c > 126u){
            continue;
        }

        qos_putc((char)c);
        last_space = 0;
    }
    qos_puts("\n");
}

static int http_fetch_raw(const char* host, const char* path, unsigned char* resp, int resp_cap){
    unsigned char ip[4];
    qos_sockaddr_in_t sa;
    char req[REQ_CAP];
    int rq = 0;

    int dns_rc = qos_dns_resolve_a_socket(host, g_dns_server, ip, DNS_TIMEOUT_MS);
    if (dns_rc != 0){
        qos_puts("DNS resolve failed.\n");
        return -1;
    }

    qos_puts("Resolved ");
    qos_puts(host);
    qos_puts(" -> ");
    print_ip4(ip);
    qos_puts("\n");

    int fd = qos_socket(QOS_AF_INET, QOS_SOCK_STREAM, 0);
    if (fd < 0){
        qos_puts("socket() failed.\n");
        return -1;
    }

    (void)qos_socket_set_nonblocking(fd, 0);
    (void)qos_socket_set_recv_timeout(fd, RECV_TIMEOUT_MS);

    sa.family = QOS_AF_INET;
    sa.port = 80u;
    sa.addr[0] = ip[0];
    sa.addr[1] = ip[1];
    sa.addr[2] = ip[2];
    sa.addr[3] = ip[3];
    for (int i = 0; i < (int)sizeof(sa.reserved); i++){
        sa.reserved[i] = 0;
    }

    if (qos_connect(fd, &sa, (unsigned int)sizeof(sa)) != 0){
        qos_puts("connect() failed.\n");
        (void)qos_close(fd);
        return -1;
    }

    if (append_str(req, REQ_CAP, &rq, "GET ") != 0 ||
        append_str(req, REQ_CAP, &rq, (path && *path) ? path : "/") != 0 ||
        append_str(req, REQ_CAP, &rq, " HTTP/1.1\r\nHost: ") != 0 ||
        append_str(req, REQ_CAP, &rq, host) != 0 ||
        append_str(req, REQ_CAP, &rq, "\r\nUser-Agent: QOS-WebBrowser/0.1\r\nConnection: close\r\n\r\n") != 0){
        qos_puts("Request build failed.\n");
        (void)qos_close(fd);
        return -1;
    }

    if (qos_send(fd, req, (unsigned int)rq, 0) < 0){
        qos_puts("send() failed.\n");
        (void)qos_close(fd);
        return -1;
    }

    int total = 0;
    while (total < resp_cap){
        int n = qos_recv(fd, &resp[total], (unsigned int)(resp_cap - total), QOS_SOCK_TIMEOUT_USE_SOCKET);
        if (n == QOS_SOCK_ERR_AGAIN){
            continue;
        }
        if (n <= 0){
            break;
        }
        total += n;
    }

    (void)qos_close(fd);
    return total;
}

static void cmd_help(void){
    qos_puts("Commands:\n");
    qos_puts(" help\n");
    qos_puts(" open <host> [path]\n");
    qos_puts(" exit\n");
}

static void cmd_open(const char* host, const char* path){
    static unsigned char resp[RESP_CAP];
    int n;

    if (!host || !*host){
        qos_puts("Usage: open <host> [path]\n");
        return;
    }

    qos_puts("Fetching http://");
    qos_puts(host);
    qos_puts((path && *path) ? path : "/");
    qos_puts("\n");

    n = http_fetch_raw(host, path, resp, (int)sizeof(resp));
    if (n <= 0){
        qos_puts("HTTP fetch failed or empty.\n");
        return;
    }

    qos_puts("Received bytes=");
    print_uint((unsigned int)n);
    qos_puts("\n\n");

    int body = find_http_body(resp, n);
    print_html_text(&resp[body], n - body);
}

static void print_prompt(void){
    qos_puts("\nWEB> ");
}

static void execute_line(void){
    if (g_input_len <= 0){
        return;
    }
    g_input[g_input_len] = 0;

    if (str_eq(g_input, "help")){
        cmd_help();
        return;
    }
    if (str_eq(g_input, "exit")){
        (void)qos_tty_set_owner(0);
        qos_exit(0);
    }

    if (str_starts_with(g_input, "open ")){
        char* p = g_input + 5;
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
            qos_puts("Usage: open <host> [path]\n");
            return;
        }
        cmd_open(host, path);
        return;
    }

    qos_puts("Unknown command. Type 'help'.\n");
}

void program_main(void){
    qos_puts("WebBrowser (text mode) ready.\n");
    qos_puts("Type 'help' for commands.\n");
    print_prompt();

    while (1){
        int ch = qos_try_getc();
        if (ch < 0){
            continue;
        }

        if (ch == '\r' || ch == '\n'){
            qos_puts("\n");
            execute_line();
            g_input_len = 0;
            g_input[0] = 0;
            print_prompt();
            continue;
        }

        if (ch == 127 || ch == '\b'){
            if (g_input_len > 0){
                g_input_len--;
                g_input[g_input_len] = 0;
                qos_puts("\b \b");
            }
            continue;
        }

        if (g_input_len < (INPUT_CAP - 1)){
            g_input[g_input_len++] = (char)ch;
            qos_putc((char)ch);
        }
    }
}
