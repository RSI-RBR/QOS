#include "syscall.h"
#include "user_net.h"

#define BUF_SIZE 128
#define LOGIN_BUF_SIZE 64
#define LOGIN_MAX_TRIES 3

static char g_buf[BUF_SIZE];
static char g_local_buf[BUF_SIZE];
static int g_local_len = 0;
static char g_remote_buf[BUF_SIZE];
static int g_remote_len = 0;
static int g_tty_owned = 1;
static int g_shell_pid = -1;
static char g_login_user[LOGIN_BUF_SIZE];
static char g_login_pass[LOGIN_BUF_SIZE];
static unsigned int g_login_user_len = 0u;
static unsigned int g_login_pass_len = 0u;
static unsigned int g_local_login_attempts = 0u;
static int g_local_login_stage = 0; /* 0=needs prompt, 1=username, 2=password */
static int g_local_authed = 0;
static int g_local_locked = 0;

static void print_prompt(void);

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

static unsigned int str_len(const char* s){
    unsigned int n = 0;
    if (!s){
        return 0;
    }
    while (s[n]){
        n++;
    }
    return n;
}

static char* skip_spaces(char* p){
    if (!p){
        return 0;
    }
    while (*p == ' '){
        p++;
    }
    return p;
}

static char* parse_arg_token(char** io){
    char* p = 0;
    char* start = 0;
    if (!io || !*io){
        return 0;
    }
    p = skip_spaces(*io);
    if (!p || !*p){
        *io = p;
        return 0;
    }
    if (*p == '"'){
        p++;
        start = p;
        while (*p && *p != '"'){
            p++;
        }
        if (*p != '"'){
            *io = p;
            return 0;
        }
        *p++ = 0;
        *io = p;
        return start;
    }
    start = p;
    while (*p && *p != ' '){
        p++;
    }
    if (*p){
        *p++ = 0;
    }
    *io = p;
    return start;
}

static int mem_eq(const unsigned char* a, const unsigned char* b, unsigned int n){
    unsigned char diff = 0;
    for (unsigned int i = 0; i < n; i++){
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int parse_uint(const char* s, unsigned int* out){
    unsigned int v = 0;
    unsigned int n = 0;
    if (!s || !*s || !out){
        return -1;
    }
    while (s[n]){
        char c = s[n];
        if (c < '0' || c > '9'){
            return -1;
        }
        v = (v * 10u) + (unsigned int)(c - '0');
        if (v > 1000u){
            return -1;
        }
        n++;
    }
    *out = v;
    return 0;
}

static void secure_zero(char* buf, unsigned int n){
    volatile char* p = (volatile char*)buf;
    if (!p){
        return;
    }
    for (unsigned int i = 0; i < n; i++){
        p[i] = 0;
    }
}

static int remote_login_has_authed_tty(void){
    unsigned int st = qos_remote_login_state();
    unsigned int need = QOS_RLOGIN_STATE_AUTHED | QOS_RLOGIN_STATE_TTY_ATTACHED;
    return ((st & need) == need) ? 1 : 0;
}

static void local_login_prompt_username(void){
    qos_puts("\nLocal login required.\n");
    qos_puts("login: ");
    g_login_user_len = 0u;
    g_login_user[0] = 0;
    g_local_login_stage = 1;
}

static void local_login_prompt_password(void){
    qos_puts("password: ");
    g_login_pass_len = 0u;
    g_login_pass[0] = 0;
    g_local_login_stage = 2;
}

static void local_login_reset_sensitive(void){
    secure_zero(g_login_user, sizeof(g_login_user));
    secure_zero(g_login_pass, sizeof(g_login_pass));
    g_login_user_len = 0u;
    g_login_pass_len = 0u;
}

static void local_login_init(void){
    g_local_authed = 0;
    g_local_locked = 0;
    g_local_login_attempts = 0u;
    g_local_login_stage = 0;
    local_login_reset_sensitive();

    if (qos_auth_is_ready() == 0){
        g_local_locked = 1;
        qos_puts("Local login unavailable: auth is not ready.\n");
        return;
    }
    {
        char expected_user[LOGIN_BUF_SIZE];
        if (qos_auth_get_username(expected_user, sizeof(expected_user)) != 0 || expected_user[0] == 0){
            g_local_locked = 1;
            qos_puts("Local login unavailable: username read failed.\n");
        } else{
            qos_puts("Local console login required (UART/USB input).\n");
        }
    }
}

static void local_login_handle_char(int ch){
    if (g_local_authed || g_local_locked){
        return;
    }
    if (g_local_login_stage == 0){
        local_login_prompt_username();
    }

    if (g_local_login_stage == 1){
        if (ch == '\r' || ch == '\n'){
            g_login_user[g_login_user_len] = 0;
            qos_puts("\n");
            local_login_prompt_password();
            return;
        }
        if (ch == 127 || ch == '\b'){
            if (g_login_user_len > 0u){
                g_login_user_len--;
                g_login_user[g_login_user_len] = 0;
                qos_puts("\b \b");
            }
            return;
        }
        if (ch < 32 || ch > 126){
            return;
        }
        if (g_login_user_len + 1u < (unsigned int)sizeof(g_login_user)){
            g_login_user[g_login_user_len++] = (char)ch;
            g_login_user[g_login_user_len] = 0;
            qos_putc((char)ch);
        }
        return;
    }

    if (g_local_login_stage == 2){
        if (ch == '\r' || ch == '\n'){
            g_login_pass[g_login_pass_len] = 0;
            qos_puts("\n");
            if (qos_auth_verify_password(g_login_user, g_login_pass) == 0){
                g_local_authed = 1;
                g_local_login_attempts = 0u;
                local_login_reset_sensitive();
                qos_puts("Access granted.\n");
                print_prompt();
                return;
            }
            local_login_reset_sensitive();
            g_local_login_attempts++;
            qos_puts("Access denied.\n");
            if (g_local_login_attempts >= LOGIN_MAX_TRIES){
                g_local_locked = 1;
                qos_puts("Local console login locked after too many attempts.\n");
                return;
            }
            g_local_login_stage = 0;
            return;
        }
        if (ch == 127 || ch == '\b'){
            if (g_login_pass_len > 0u){
                g_login_pass_len--;
                g_login_pass[g_login_pass_len] = 0;
                qos_puts("\b \b");
            }
            return;
        }
        if (ch < 32 || ch > 126){
            return;
        }
        if (g_login_pass_len + 1u < (unsigned int)sizeof(g_login_pass)){
            g_login_pass[g_login_pass_len++] = (char)ch;
            g_login_pass[g_login_pass_len] = 0;
            qos_putc('*');
        }
    }
}

static void print_prompt(void){
    qos_puts("\nUQOS> ");
}

static int shell_claim_tty(void){
    int rc = qos_tty_claim_self();
    if (rc == 0){
        g_tty_owned = 1;
        if (g_shell_pid < 0){
            g_shell_pid = qos_getpid();
        }
    }
    return rc;
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

static void print_int(int v){
    if (v < 0){
        qos_putc('-');
        unsigned int mag = (unsigned int)(-(long long)v);
        print_uint(mag);
        return;
    }
    print_uint((unsigned int)v);
}

static void print_hex32(unsigned int v){
    static const char hexdigits[] = "0123456789ABCDEF";
    qos_puts("0x");
    for (int i = 7; i >= 0; i--){
        unsigned int nibble = (v >> ((unsigned int)i * 4u)) & 0xFu;
        qos_putc(hexdigits[nibble]);
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

static int parse_ip4(const char* s, unsigned char out[4]){
    if (!s || !out){
        return -1;
    }
    for (unsigned int part = 0; part < 4u; part++){
        unsigned int v = 0;
        unsigned int digits = 0;
        while (*s >= '0' && *s <= '9'){
            v = (v * 10u) + (unsigned int)(*s - '0');
            if (v > 255u){
                return -1;
            }
            s++;
            digits++;
        }
        if (digits == 0){
            return -1;
        }
        out[part] = (unsigned char)v;
        if (part < 3u){
            if (*s != '.'){
                return -1;
            }
            s++;
        } else if (*s != 0){
            return -1;
        }
    }
    return 0;
}

static const unsigned char g_dns_server[4] = {10, 0, 0, 1};
static const char g_wifi_fw_83[] = "4343WIFIBIN";
static const char g_wifi_nv_83[] = "4343NVRMTXT";

static int dns_resolve_a(const char* host, unsigned char out_ip[4], int verbose){
    int rc = qos_dns_resolve_a_secure_socket(host, out_ip, 4500u);
    if (rc == 0){
        if (verbose){
            qos_puts("A ");
            print_ip4(out_ip);
            qos_puts(" (DoH/TLS)\n");
        }
        return 0;
    }

    rc = qos_dns_resolve_a_socket(host, g_dns_server, out_ip, 3000u);
    if (rc == 0){
        if (verbose){
            qos_puts("A ");
            print_ip4(out_ip);
            qos_puts(" (fallback UDP)\n");
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
    qos_puts(" game\n");
    qos_puts(" web\n");
    qos_puts(" tty\n");
    qos_puts(" chvt <0-3>\n");
    qos_puts(" termout both|uart|hdmi|status\n");
    qos_puts(" dma on|off|status\n");
    qos_puts(" securitylog\n");
    qos_puts(" ps\n");
    qos_puts(" validate\n");
    qos_puts(" clear\n");
    qos_puts(" fbinfo\n");
    qos_puts(" usbstat\n");
    qos_puts(" netstat\n");
    qos_puts(" rloginstat\n");
    qos_puts(" ip\n");
    qos_puts(" setip <a.b.c.d>\n");
    qos_puts(" setgw <a.b.c.d>\n");
    qos_puts(" ping\n");
    qos_puts(" dnscheck <domain>\n");
    qos_puts(" httpget <host> [path]\n");
    qos_puts(" tlstest\n");
    qos_puts(" wifiinit        - SDIO bus probe only; reboot before wifiload\n");
    qos_puts(" wifiload [fw83 nv83] - read firmware first, then init WiFi\n");
    qos_puts(" wifiup          - release WiFi firmware and enable data path\n");
    qos_puts(" wifidown\n");
    qos_puts(" wifistat\n");
    qos_puts(" wifiver\n");
    qos_puts(" wifiscan\n");
    qos_puts(" wifiscanx [passes]\n");
    qos_puts(" wifijoin <ssid> <password>   (quotes allowed)\n");
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

static void cmd_game(void){
    // FAT 8.3 uppercase, space-padded: "GAME    BIN"
    static const char game_file_83[] = "GAME    BIN";
    int pid = qos_run_program_named(game_file_83);
    if (pid < 0){
        qos_puts("GAME.BIN load failed.\n");
        return;
    }
    if (qos_tty_set_owner(pid) != 0){
        qos_puts("Warning: could not transfer TTY ownership.\n");
    } else{
        g_tty_owned = 0;
    }
    qos_puts("Game queued as PID ");
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

static void cmd_rloginstat(void){
    qos_remote_login_dump_stats();
}

static void cmd_ip(void){
    unsigned char ip[4];
    unsigned char gw[4];
    if (qos_net_get_local_ip(ip) != 0){
        qos_puts("IP unavailable.\n");
        return;
    }
    if (qos_net_get_gateway_ip(gw) != 0){
        qos_puts("Gateway unavailable.\n");
        return;
    }
    qos_puts("Local IP ");
    print_ip4(ip);
    qos_puts(" gateway ");
    print_ip4(gw);
    qos_puts("\n");
}

static void cmd_setip(const char* s){
    unsigned char ip[4];
    if (!s || parse_ip4(s, ip) != 0){
        qos_puts("Usage: setip <a.b.c.d>\n");
        return;
    }
    if (qos_net_set_local_ip(ip) != 0){
        qos_puts("setip failed.\n");
        return;
    }
    qos_puts("Local IP set to ");
    print_ip4(ip);
    qos_puts("\n");
}

static void cmd_setgw(const char* s){
    unsigned char ip[4];
    if (!s || parse_ip4(s, ip) != 0){
        qos_puts("Usage: setgw <a.b.c.d>\n");
        return;
    }
    if (qos_net_set_gateway_ip(ip) != 0){
        qos_puts("setgw failed.\n");
        return;
    }
    qos_puts("Gateway set to ");
    print_ip4(ip);
    qos_puts("\n");
}

static void cmd_ps(void){
    qos_process_dump();
}

static void cmd_tty(void){
    int active = qos_term_get_active();
    int owner = qos_tty_get_owner();
    qos_puts("active tty");
    print_int(active);
    qos_puts(" owner pid=");
    print_int(owner);
    qos_puts("\n");
}

static void cmd_chvt(unsigned int id){
    if (id >= 4u){
        qos_puts("Usage: chvt <0-3>\n");
        return;
    }
    if (qos_term_switch((int)id) != 0){
        qos_puts("chvt failed.\n");
        return;
    }
    qos_puts("Switched to tty");
    print_uint(id);
    qos_puts(".\n");
}

static void cmd_termout(const char* mode){
    unsigned int flags;
    if (!mode || !*mode || str_eq(mode, "status")){
        flags = qos_term_get_output();
        qos_puts("terminal output=");
        if ((flags & QOS_TERM_OUTPUT_UART) && (flags & QOS_TERM_OUTPUT_FB)){
            qos_puts("both");
        } else if (flags & QOS_TERM_OUTPUT_UART){
            qos_puts("uart");
        } else if (flags & QOS_TERM_OUTPUT_FB){
            qos_puts("hdmi");
        } else{
            qos_puts("none");
        }
        qos_puts("\n");
        return;
    }

    if (str_eq(mode, "both")){
        flags = QOS_TERM_OUTPUT_UART | QOS_TERM_OUTPUT_FB;
    } else if (str_eq(mode, "uart")){
        flags = QOS_TERM_OUTPUT_UART;
    } else if (str_eq(mode, "hdmi") || str_eq(mode, "fb")){
        flags = QOS_TERM_OUTPUT_FB;
    } else{
        qos_puts("Usage: termout both|uart|hdmi|status\n");
        return;
    }

    if (qos_term_set_output(flags) != 0){
        qos_puts("termout failed.\n");
        return;
    }
    qos_puts("terminal output set.\n");
}

static void cmd_dma(const char* mode){
    unsigned int st;
    if (!mode || !*mode || str_eq(mode, "status")){
        st = qos_dma_status();
        qos_puts("dma=");
        qos_puts((st & 1u) ? "on" : "off");
        qos_puts(" failures=");
        print_uint((st >> 16) & 0xFFFFu);
        qos_puts(" cs=");
        print_hex32(qos_dma_last_cs());
        qos_puts(" debug=");
        print_hex32(qos_dma_last_debug());
        qos_puts("\n");
        return;
    }
    if (str_eq(mode, "on")){
        (void)qos_dma_set_enabled(1);
        qos_puts("dma enabled for framebuffer console tests.\n");
        return;
    }
    if (str_eq(mode, "off")){
        (void)qos_dma_set_enabled(0);
        qos_puts("dma disabled; framebuffer console uses CPU copy.\n");
        return;
    }
    qos_puts("Usage: dma on|off|status\n");
}

static void cmd_securitylog(void){
    qos_security_log_dump();
}

static void cmd_ping(void){
    int rtt = qos_net_ping_gateway(1200);
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

static void cmd_tlstest(void){
    int c = -1;
    int s = -1;
    static unsigned char ch[256];
    static unsigned char sh[256];
    static unsigned char c_cipher[256];
    static unsigned char s_plain[256];
    static unsigned char s_cipher[256];
    static unsigned char c_plain[256];
    static const unsigned char msg1[] = "hello-from-client";
    static const unsigned char msg2[] = "hello-from-server";

    qos_puts("TLS test: opening sessions...\n");
    c = qos_tls_open(QOS_TLS_ROLE_CLIENT);
    s = qos_tls_open(QOS_TLS_ROLE_SERVER);
    if (c < 0 || s < 0){
        qos_puts("TLS test failed: open\n");
        if (c >= 0){
            (void)qos_tls_close(c);
        }
        if (s >= 0){
            (void)qos_tls_close(s);
        }
        return;
    }

    int ch_len = qos_tls_build_client_hello(c, ch, sizeof(ch));
    if (ch_len <= 0){
        qos_puts("TLS test failed: build client hello\n");
        qos_puts(" ch_len=");
        print_int(ch_len);
        qos_puts("\n");
        goto out;
    }
    int sh_len = qos_tls_process_client_hello_build_server_hello(
        s, ch, (unsigned int)ch_len, sh, sizeof(sh));
    if (sh_len <= 0){
        qos_puts("TLS test failed: server process/build\n");
        qos_puts(" sh_len=");
        print_int(sh_len);
        qos_puts("\n");
        goto out;
    }
    if (qos_tls_process_server_hello(c, sh, (unsigned int)sh_len) != 0){
        qos_puts("TLS test failed: client process server hello\n");
        goto out;
    }
    if (!qos_tls_is_ready(c) || !qos_tls_is_ready(s)){
        qos_puts("TLS test failed: session not ready\n");
        goto out;
    }

    unsigned int msg1_len = str_len((const char*)msg1);
    unsigned int msg2_len = str_len((const char*)msg2);

    qos_tls_record_io_t io1;
    io1.inner_type = QOS_TLS_RECORD_INNER_APPDATA;
    io1.in = msg1;
    io1.in_len = msg1_len;
    io1.out = c_cipher;
    io1.out_cap = sizeof(c_cipher);
    io1.out_len = 0;
    int enc1 = qos_tls_record_encrypt(c, &io1);
    if (enc1 <= 0){
        qos_puts("TLS test failed: c->s encrypt\n");
        goto out;
    }

    qos_tls_record_io_t io2;
    io2.inner_type = 0;
    io2.in = c_cipher;
    io2.in_len = (unsigned int)enc1;
    io2.out = s_plain;
    io2.out_cap = sizeof(s_plain);
    io2.out_len = 0;
    int dec1 = qos_tls_record_decrypt(s, &io2);
    if (dec1 <= 0 || (unsigned int)dec1 != msg1_len ||
        io2.inner_type != QOS_TLS_RECORD_INNER_APPDATA ||
        !mem_eq(s_plain, msg1, (unsigned int)dec1)){
        qos_puts("TLS test failed: c->s decrypt/verify\n");
        qos_puts(" enc1=");
        print_int(enc1);
        qos_puts(" dec1=");
        print_int(dec1);
        qos_puts(" exp=");
        print_uint(msg1_len);
        qos_puts(" inner=");
        print_uint((unsigned int)io2.inner_type);
        qos_puts("\n");
        goto out;
    }

    qos_tls_record_io_t io3;
    io3.inner_type = QOS_TLS_RECORD_INNER_APPDATA;
    io3.in = msg2;
    io3.in_len = msg2_len;
    io3.out = s_cipher;
    io3.out_cap = sizeof(s_cipher);
    io3.out_len = 0;
    int enc2 = qos_tls_record_encrypt(s, &io3);
    if (enc2 <= 0){
        qos_puts("TLS test failed: s->c encrypt\n");
        goto out;
    }

    qos_tls_record_io_t io4;
    io4.inner_type = 0;
    io4.in = s_cipher;
    io4.in_len = (unsigned int)enc2;
    io4.out = c_plain;
    io4.out_cap = sizeof(c_plain);
    io4.out_len = 0;
    int dec2 = qos_tls_record_decrypt(c, &io4);
    if (dec2 <= 0 || (unsigned int)dec2 != msg2_len ||
        io4.inner_type != QOS_TLS_RECORD_INNER_APPDATA ||
        !mem_eq(c_plain, msg2, (unsigned int)dec2)){
        qos_puts("TLS test failed: s->c decrypt/verify\n");
        qos_puts(" enc2=");
        print_int(enc2);
        qos_puts(" dec2=");
        print_int(dec2);
        qos_puts(" exp=");
        print_uint(msg2_len);
        qos_puts(" inner=");
        print_uint((unsigned int)io4.inner_type);
        qos_puts("\n");
        goto out;
    }

    qos_puts("TLS test OK: handshake + encrypted records\n");

out:
    (void)qos_tls_close(c);
    (void)qos_tls_close(s);
}

static void cmd_wifiinit(void){
    int rc = qos_wifi_init();
    if (rc == 0){
        qos_puts("WiFi SDIO init OK\n");
    } else{
        qos_puts("WiFi SDIO init failed\n");
    }
}

static void cmd_wifiload(const char* fw83, const char* nv83){
    const char* fw = (fw83 && *fw83) ? fw83 : g_wifi_fw_83;
    const char* nv = (nv83 && *nv83) ? nv83 : g_wifi_nv_83;
    int rc = qos_wifi_load_fw(fw, nv);
    if (rc == 0){
        qos_puts("WiFi firmware staged\n");
    } else{
        qos_puts("WiFi firmware stage failed\n");
    }
}

static void cmd_wifiup(void){
    int rc = qos_wifi_up();
    if (rc == 0){
        qos_puts("WiFi UP OK\n");
    } else{
        qos_puts("WiFi UP failed\n");
    }
}

static void cmd_wifidown(void){
    int rc = qos_wifi_down();
    if (rc == 0){
        qos_puts("WiFi DOWN OK\n");
    } else{
        qos_puts("WiFi DOWN failed\n");
    }
}

static void cmd_wifiver(void){
    char version[160];
    int rc;

    version[0] = 0;
    rc = qos_wifi_get_version(version, sizeof(version));
    if (rc == 0 && version[0]){
        qos_puts("WiFi firmware: ");
        qos_puts(version);
        qos_puts("\n");
    } else{
        qos_puts("WiFi firmware version failed\n");
    }
}

static void cmd_wifiscan(void){
    cyw43_scan_result_t results[8];
    int n = qos_wifi_scan(results, 8u);
    if (n < 0){
        qos_puts("WiFi scan failed\n");
        return;
    }
    qos_puts("WiFi scan entries=");
    print_uint((unsigned int)n);
    qos_puts("\n");
    for (int i = 0; i < n; i++){
        qos_puts(" ");
        print_uint((unsigned int)i);
        qos_puts(": ");
        qos_puts(results[i].ssid);
        qos_puts(" ch=");
        print_uint((unsigned int)results[i].channel);
        qos_puts(" rssi=");
        print_int(results[i].rssi_dbm);
        qos_puts(" auth=");
        print_uint((unsigned int)results[i].auth);
        qos_puts("\n");
    }
}

static void scan_result_copy(cyw43_scan_result_t* dst, const cyw43_scan_result_t* src){
    if (!dst || !src){
        return;
    }
    for (unsigned int i = 0; i < sizeof(dst->ssid); i++){
        dst->ssid[i] = src->ssid[i];
    }
    dst->ssid[sizeof(dst->ssid) - 1u] = 0;
    dst->channel = src->channel;
    dst->rssi_dbm = src->rssi_dbm;
    dst->auth = src->auth;
}

static int scan_result_same_ap(const cyw43_scan_result_t* a, const cyw43_scan_result_t* b){
    if (!a || !b){
        return 0;
    }
    return (a->channel == b->channel) && str_eq(a->ssid, b->ssid);
}

static void cmd_wifiscanx(unsigned int passes){
    cyw43_scan_result_t merged[32];
    unsigned int merged_n = 0;
    unsigned int ok_passes = 0;
    unsigned int fail_passes = 0;

    if (passes == 0u){
        passes = 3u;
    }
    if (passes > 10u){
        passes = 10u;
    }

    for (unsigned int p = 0; p < passes; p++){
        cyw43_scan_result_t batch[8];
        int n = qos_wifi_scan(batch, 8u);
        if (n < 0){
            fail_passes++;
            continue;
        }
        ok_passes++;

        for (int i = 0; i < n; i++){
            int found = -1;
            for (unsigned int j = 0; j < merged_n; j++){
                if (scan_result_same_ap(&merged[j], &batch[i])){
                    found = (int)j;
                    break;
                }
            }

            if (found < 0){
                if (merged_n < (unsigned int)(sizeof(merged) / sizeof(merged[0]))){
                    scan_result_copy(&merged[merged_n], &batch[i]);
                    merged_n++;
                }
            } else{
                if (batch[i].rssi_dbm > merged[found].rssi_dbm){
                    merged[found].rssi_dbm = batch[i].rssi_dbm;
                }
                if (batch[i].auth > merged[found].auth){
                    merged[found].auth = batch[i].auth;
                }
            }
        }
    }

    for (unsigned int i = 0; i + 1u < merged_n; i++){
        for (unsigned int j = i + 1u; j < merged_n; j++){
            if (merged[j].rssi_dbm > merged[i].rssi_dbm){
                cyw43_scan_result_t tmp;
                scan_result_copy(&tmp, &merged[i]);
                scan_result_copy(&merged[i], &merged[j]);
                scan_result_copy(&merged[j], &tmp);
            }
        }
    }

    qos_puts("WiFi scanx passes=");
    print_uint(passes);
    qos_puts(" ok=");
    print_uint(ok_passes);
    qos_puts(" fail=");
    print_uint(fail_passes);
    qos_puts(" unique=");
    print_uint(merged_n);
    qos_puts("\n");

    for (unsigned int i = 0; i < merged_n; i++){
        qos_puts(" ");
        print_uint(i);
        qos_puts(": ");
        qos_puts(merged[i].ssid);
        qos_puts(" ch=");
        print_uint((unsigned int)merged[i].channel);
        qos_puts(" rssi=");
        print_int(merged[i].rssi_dbm);
        qos_puts(" auth=");
        print_uint((unsigned int)merged[i].auth);
        qos_puts("\n");
    }
}

static void cmd_wifijoin(const char* ssid, const char* password){
    int rc;
    if (!ssid || !*ssid || !password || !*password){
        qos_puts("Usage: wifijoin <ssid> <password>\n");
        qos_puts("   or: wifijoin \"ssid with spaces\" \"password with spaces\"\n");
        return;
    }
    rc = qos_wifi_join(ssid, password);
    if (rc == 0){
        qos_puts("WiFi join OK\n");
    } else{
        qos_puts("WiFi join failed\n");
    }
}

static void execute_line(void){
    unsigned int line_len = str_len(g_buf);
    if (line_len == 0u){
        return;
    }
    g_buf[line_len] = 0;

    if (str_eq(g_buf, "help")){
        cmd_help();
    } else if (str_eq(g_buf, "run")){
        cmd_run();
    } else if (str_eq(g_buf, "game")){
        cmd_game();
    } else if (str_eq(g_buf, "web")){
        cmd_web();
    } else if (str_eq(g_buf, "tty")){
        cmd_tty();
    } else if (str_starts_with(g_buf, "chvt ")){
        const char* p = g_buf + 5;
        unsigned int id = 0;
        while (*p == ' '){
            p++;
        }
        if (parse_uint(p, &id) != 0){
            qos_puts("Usage: chvt <0-3>\n");
        } else{
            cmd_chvt(id);
        }
    } else if (str_eq(g_buf, "chvt")){
        qos_puts("Usage: chvt <0-3>\n");
    } else if (str_starts_with(g_buf, "termout ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_termout(p);
    } else if (str_eq(g_buf, "termout")){
        cmd_termout("status");
    } else if (str_starts_with(g_buf, "dma ")){
        const char* p = g_buf + 4;
        while (*p == ' '){
            p++;
        }
        cmd_dma(p);
    } else if (str_eq(g_buf, "dma")){
        cmd_dma("status");
    } else if (str_eq(g_buf, "securitylog")){
        cmd_securitylog();
    } else if (str_eq(g_buf, "clear")){
        qos_term_clear();
    } else if (str_eq(g_buf, "fbinfo")){
        cmd_fbinfo();
    } else if (str_eq(g_buf, "usbstat")){
        cmd_usbstat();
    } else if (str_eq(g_buf, "netstat")){
        cmd_netstat();
    } else if (str_eq(g_buf, "rloginstat")){
        cmd_rloginstat();
    } else if (str_eq(g_buf, "ip")){
        cmd_ip();
    } else if (str_starts_with(g_buf, "setip ")){
        const char* p = g_buf + 6;
        while (*p == ' '){
            p++;
        }
        cmd_setip(p);
    } else if (str_eq(g_buf, "setip")){
        qos_puts("Usage: setip <a.b.c.d>\n");
    } else if (str_starts_with(g_buf, "setgw ")){
        const char* p = g_buf + 6;
        while (*p == ' '){
            p++;
        }
        cmd_setgw(p);
    } else if (str_eq(g_buf, "setgw")){
        qos_puts("Usage: setgw <a.b.c.d>\n");
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
    } else if (str_eq(g_buf, "tlstest")){
        cmd_tlstest();
    } else if (str_eq(g_buf, "wifiinit")){
        cmd_wifiinit();
    } else if (str_starts_with(g_buf, "wifiload ")){
        char* p = g_buf + 9;
        char* fw = 0;
        char* nv = 0;
        while (*p == ' '){
            p++;
        }
        if (*p){
            fw = p;
            while (*p && *p != ' '){
                p++;
            }
            if (*p){
                *p++ = 0;
                while (*p == ' '){
                    p++;
                }
                if (*p){
                    nv = p;
                }
            }
        }
        cmd_wifiload(fw, nv);
    } else if (str_eq(g_buf, "wifiload")){
        cmd_wifiload(0, 0);
    } else if (str_eq(g_buf, "wifiup")){
        cmd_wifiup();
    } else if (str_eq(g_buf, "wifidown")){
        cmd_wifidown();
    } else if (str_eq(g_buf, "wifistat")){
        qos_wifi_dump_status();
    } else if (str_eq(g_buf, "wifiver")){
        cmd_wifiver();
    } else if (str_eq(g_buf, "wifiscan")){
        cmd_wifiscan();
    } else if (str_starts_with(g_buf, "wifiscanx ")){
        const char* p = g_buf + 10;
        unsigned int passes = 0;
        while (*p == ' '){
            p++;
        }
        if (parse_uint(p, &passes) != 0){
            qos_puts("Usage: wifiscanx [passes]\n");
        } else{
            cmd_wifiscanx(passes);
        }
    } else if (str_eq(g_buf, "wifiscanx")){
        cmd_wifiscanx(3u);
    } else if (str_starts_with(g_buf, "wifijoin ")){
        char* p = g_buf + 9;
        char* ssid = parse_arg_token(&p);
        char* password = parse_arg_token(&p);
        char* extra = parse_arg_token(&p);
        if (extra && *extra){
            qos_puts("Usage: wifijoin <ssid> <password>\n");
            qos_puts("   or: wifijoin \"ssid with spaces\" \"password with spaces\"\n");
        } else{
            cmd_wifijoin(ssid, password);
        }
    } else if (str_eq(g_buf, "wifijoin")){
        qos_puts("Usage: wifijoin <ssid> <password>\n");
        qos_puts("   or: wifijoin \"ssid with spaces\" \"password with spaces\"\n");
    } else{
        qos_puts("Unknown command.\n");
    }
}

static void execute_source_buffer(char* src_buf, int* src_len){
    if (!src_buf || !src_len){
        return;
    }
    if (*src_len <= 0){
        return;
    }
    if (*src_len >= BUF_SIZE){
        *src_len = BUF_SIZE - 1;
    }
    for (int i = 0; i < *src_len; i++){
        g_buf[i] = src_buf[i];
    }
    g_buf[*src_len] = 0;
    execute_line();
    *src_len = 0;
    src_buf[0] = 0;
}

static void shell_handle_input_char(char* src_buf, int* src_len, int ch){
    if (!src_buf || !src_len){
        return;
    }

    if (ch == '\r' || ch == '\n'){
        qos_puts("\n");
        execute_source_buffer(src_buf, src_len);
        print_prompt();
        return;
    }

    if (ch == 127 || ch == '\b'){
        if (*src_len > 0){
            (*src_len)--;
            src_buf[*src_len] = 0;
            qos_puts("\b \b");
        }
        return;
    }

    if (ch < 32 || ch > 126){
        return;
    }

    if (*src_len < (BUF_SIZE - 1)){
        src_buf[*src_len] = (char)ch;
        (*src_len)++;
        src_buf[*src_len] = 0;
        qos_putc((char)ch);
    }
}

void program_main(void){
    g_shell_pid = qos_getpid();
    if (g_shell_pid < 0){
        g_shell_pid = -1;
    }
    // Claim foreground console ownership explicitly on startup.
    (void)shell_claim_tty();
    local_login_init();

    qos_puts("User shell ready.");
    print_prompt();

    while (1){
        if (g_shell_pid < 0){
            g_shell_pid = qos_getpid();
        }
        int owner = qos_tty_get_owner();
        int shell_has_tty = (g_shell_pid >= 0 && owner == g_shell_pid);
        // Recover ownership when console is unowned or owned by a dead/exited task.
        if (!shell_has_tty){
            if (shell_claim_tty() == 0){
                owner = qos_tty_get_owner();
                shell_has_tty = (g_shell_pid >= 0 && owner == g_shell_pid);
            }
        }
        if (shell_has_tty && !g_tty_owned){
            g_tty_owned = 1;
            qos_puts("\nReturned to shell.");
            print_prompt();
        } else if (!shell_has_tty){
            g_tty_owned = 0;
            // Avoid tight spin during ownership handoff races.
            qos_sleep(1);
            continue;
        }

        qos_input_event_t ev;
        int ch = qos_try_getc_ex(&ev);
        if (ch < 0){
            // Avoid turning an idle prompt into a hot loop that starves peers.
            qos_sleep(4);
            continue;
        }

        if (ev.source == QOS_INPUT_SRC_REMOTE){
            if (!remote_login_has_authed_tty()){
                continue;
            }
            shell_handle_input_char(g_remote_buf, &g_remote_len, ch);
            continue;
        }

        if (!g_local_authed){
            local_login_handle_char(ch);
            continue;
        }

        shell_handle_input_char(g_local_buf, &g_local_len, ch);
    }
}
