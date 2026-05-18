#include "syscall.h"
#include "user_net.h"

#define BUF_SIZE 128
#define LOGIN_BUF_SIZE 64
#define LOGIN_MAX_TRIES 3
#define LOG_BUF_SIZE 2048

static char g_buf[BUF_SIZE];
static char g_local_buf[BUF_SIZE];
static int g_local_len = 0;
static char g_remote_buf[BUF_SIZE];
static char g_log_buf[LOG_BUF_SIZE];
static int g_remote_len = 0;
static int g_tty_owned = 1;
static int g_shell_pid = -1;
static int g_foreground_pid = -1;
static char g_login_user[LOGIN_BUF_SIZE];
static char g_login_pass[LOGIN_BUF_SIZE];
static unsigned int g_login_user_len = 0u;
static unsigned int g_login_pass_len = 0u;
static unsigned int g_local_login_attempts = 0u;
static int g_local_login_stage = 0; /* 0=needs prompt, 1=username, 2=password */
static int g_local_authed = 0;
static int g_local_locked = 0;

static void print_prompt(void);

static int shell_can_use_input_overlay(void){
    return g_local_authed && g_tty_owned;
}

static int shell_set_input_overlay_from(const char* src_buf, int src_len){
    char line[BUF_SIZE + 8];
    const char prefix[] = "UQOS> ";
    unsigned int pos = 0u;
    if (!shell_can_use_input_overlay()){
        return -1;
    }
    for (unsigned int i = 0u; prefix[i] && pos + 1u < sizeof(line); i++){
        line[pos++] = prefix[i];
    }
    if (src_buf && src_len > 0){
        for (int i = 0; i < src_len && pos + 1u < sizeof(line); i++){
            line[pos++] = src_buf[i];
        }
    }
    line[pos] = 0;
    return qos_term_set_input_line(line);
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
        if (v > 10000u){
            return -1;
        }
        n++;
    }
    *out = v;
    return 0;
}

static int parse_hex32_arg(const char* s, unsigned int* out){
    unsigned int v = 0u;
    unsigned int n = 0u;
    if (!s || !*s || !out){
        return -1;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')){
        s += 2;
    }
    if (!*s){
        return -1;
    }
    while (*s){
        unsigned int d;
        char c = *s++;
        if (c >= '0' && c <= '9'){
            d = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f'){
            d = (unsigned int)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F'){
            d = (unsigned int)(c - 'A') + 10u;
        } else{
            return -1;
        }
        if (n >= 8u){
            return -1;
        }
        v = (v << 4) | d;
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
    if (shell_can_use_input_overlay()){
        if (shell_set_input_overlay_from(g_local_buf, g_local_len) != 0){
            qos_puts("\nUQOS> ");
        }
    } else{
        qos_puts("\nUQOS> ");
    }
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

static void print_hex8(unsigned int v){
    static const char hexdigits[] = "0123456789ABCDEF";
    qos_putc(hexdigits[(v >> 4u) & 0xFu]);
    qos_putc(hexdigits[v & 0xFu]);
}

static unsigned int read_le16_shell(const unsigned char* p){
    return ((unsigned int)p[0]) | ((unsigned int)p[1] << 8);
}

static void print_3digits(unsigned int v){
    v %= 1000u;
    qos_putc((char)('0' + (v / 100u)));
    qos_putc((char)('0' + ((v / 10u) % 10u)));
    qos_putc((char)('0' + (v % 10u)));
}

static void print_mhz(unsigned int hz){
    print_uint(hz / 1000000u);
    qos_putc('.');
    print_3digits((hz % 1000000u) / 1000u);
    qos_puts(" MHz");
}

static void print_temp_millic(unsigned int milli_c){
    print_uint(milli_c / 1000u);
    qos_putc('.');
    print_3digits(milli_c % 1000u);
    qos_puts(" C");
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
#if defined(QOS_BOARD_PI_ZERO2W)
static const char g_wifi_fw_83[] = "P0NEXMONBIN";
static const char g_wifi_nv_83[] = "P0NEXMONTXT";
static const char g_wifi_clm_83[] = "P0NEXMONCLM";
#else
static const char g_wifi_fw_83[] = "4343WIFIBIN";
static const char g_wifi_nv_83[] = "4343NVRMTXT";
static const char g_wifi_clm_83[] = "";
#endif

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
    qos_puts(" runbg\n");
    qos_puts(" exit [pid]\n");
    qos_puts(" log [pid]\n");
    qos_puts(" gfx <pid>\n");
    qos_puts(" game\n");
    qos_puts(" scanner        - start continuous WiFi scanner (live shell output)\n");
    qos_puts(" scanlog counts|aps|handshakes\n");
    qos_puts(" web\n");
    qos_puts(" tty\n");
    qos_puts(" chvt <0-3>\n");
    qos_puts(" termout both|uart|hdmi|status\n");
    qos_puts(" dma on|off|status\n");
    qos_puts(" gpu on|off|status\n");
    qos_puts(" vsync on|off|status\n");
    qos_puts(" gpu2d status|qpu on|qpu off|v3d on|v3d off\n");
    qos_puts(" v3d probe|status|noop [0|1]|qpu|qpuwrite|qpuexec|clear <rgba32hex>\n");
    qos_puts(" gfxstat [reset]\n");
    qos_puts(" sysstat\n");
    qos_puts(" clock status|fast|normal|arm <mhz>|core <mhz>|v3d <mhz>\n");
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
    qos_puts(" wifiload [fw83 nv83 [clm83]] - read firmware first, then init WiFi\n");
    qos_puts(" wifiup          - release WiFi firmware and enable data path\n");
    qos_puts(" wifiupmon       - minimal Nexmon/monitor bring-up path\n");
    qos_puts(" wifidown\n");
    qos_puts(" wifistat\n");
    qos_puts(" wifiver\n");
    qos_puts(" wifimac rand\n");
    qos_puts(" wifiscan\n");
    qos_puts(" wifiscanfor <ssid>\n");
    qos_puts(" wifiscanx [passes]\n");
    qos_puts(" wifijoin <ssid> <password>   (quotes allowed)\n");
    qos_puts(" wifijoinhidden <ssid> <password>   (join without scan)\n");
    qos_puts(" wifiraw on|off|stat|read|drain [ms]|scan [ms]\n");
    qos_puts(" wifimon on [channel]|off|status\n");
    qos_puts(" ledtest [blinks [on_ms off_ms]] | on | off | auto | status\n");
}

static void cmd_run(void){
    qos_puts("Loading PROGRAM.BIN...\n");
    int pid = qos_run_program();
    if (pid < 0){
        qos_puts("Program load failed.\n");
        return;
    }
    g_foreground_pid = pid;
    qos_puts("Program queued as PID ");
    print_uint((unsigned int)pid);
    qos_puts("\n");
    if (qos_display_create_graphics(pid) < 0){
        qos_puts("Warning: graphics session create failed.\n");
    }
    if (qos_display_switch_graphics(pid) != 0){
        qos_puts("Warning: graphics switch request failed.\n");
    }
}

static void cmd_runbg(void){
    qos_puts("Loading PROGRAM.BIN in background...\n");
    int pid = qos_run_program();
    if (pid < 0){
        qos_puts("Program load failed.\n");
        return;
    }
    qos_puts("Program queued in background as PID ");
    print_uint((unsigned int)pid);
    qos_puts("\n");
}

static void cmd_web(void){
    // FAT 8.3 uppercase, space-padded: "WEBBROWSBIN"
    static const char web_file_83[] = "WEBBROWSBIN";
    qos_puts("Loading WEBBROWS.BIN...\n");
    int pid = qos_run_program_named(web_file_83);
    if (pid < 0){
        qos_puts("WEBBROWS.BIN load failed.\n");
        return;
    }
    g_foreground_pid = pid;
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
    qos_puts("Loading GAME.BIN...\n");
    int pid = qos_run_program_named(game_file_83);
    if (pid < 0){
        qos_puts("GAME.BIN load failed.\n");
        return;
    }
    g_foreground_pid = pid;
    qos_puts("Game queued as PID ");
    print_uint((unsigned int)pid);
    qos_puts("\n");
    if (qos_display_create_graphics(pid) < 0){
        qos_puts("Warning: graphics session create failed.\n");
    }
    if (qos_display_switch_graphics(pid) != 0){
        qos_puts("Warning: graphics switch request failed.\n");
    }
}

static void cmd_scanner(void){
    // FAT 8.3 uppercase, space-padded: "SCANNER BIN"
    static const char scanner_file_83[] = "SCANNER BIN";
    qos_puts("Loading SCANNER.BIN...\n");
    int pid = qos_run_program_named(scanner_file_83);
    if (pid < 0){
        qos_puts("SCANNER.BIN load failed.\n");
        return;
    }
    g_foreground_pid = pid;
    qos_puts("Scanner running as PID ");
    print_uint((unsigned int)pid);
    qos_puts(" (live output in this shell)\n");
}

static const char* scanlog_path_for_arg(const char* arg){
    if (!arg || !*arg || str_eq(arg, "handshakes")){
        return "handshakes.log";
    }
    if (str_eq(arg, "counts")){
        return "counts.log";
    }
    if (str_eq(arg, "aps")){
        return "aps.log";
    }
    return 0;
}

static void cmd_scanlog(const char* arg){
    const char* path = scanlog_path_for_arg(arg);
    unsigned int off = 0u;
    unsigned int total = 0u;
    char last = '\n';
    if (!path){
        qos_puts("Usage: scanlog counts|aps|handshakes\n");
        return;
    }
    qos_puts("Scanner log ");
    qos_puts(path);
    qos_puts(":\n");
    while (1){
        int n = qos_scanner_log_read(path, off, (unsigned char*)g_log_buf, sizeof(g_log_buf) - 1u);
        if (n < 0){
            qos_puts("scanlog read failed.\n");
            return;
        }
        if (n == 0){
            break;
        }
        g_log_buf[(unsigned int)n] = 0;
        last = g_log_buf[(unsigned int)n - 1u];
        qos_puts(g_log_buf);
        off += (unsigned int)n;
        total += (unsigned int)n;
        if (total >= 65536u){
            qos_puts("\nscanlog output truncated at 65536 bytes.\n");
            break;
        }
    }
    if (total == 0u){
        qos_puts("(empty)\n");
    } else if (last != '\n'){
        qos_puts("\n");
    }
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
    if (qos_display_switch_session(id) != 0){
        qos_puts("chvt failed.\n");
        return;
    }
    if (id == 0u){
        qos_puts("Switched to tty0.\n");
        return;
    }
    qos_puts("Switched to gfx");
    print_uint(id);
    qos_puts(".\n");
}

static void cmd_gfx(unsigned int pid){
    if (qos_display_switch_graphics((int)pid) != 0){
        qos_puts("gfx switch failed.\n");
        return;
    }
    g_foreground_pid = (int)pid;
    qos_puts("Switched HDMI to graphics PID ");
    print_uint(pid);
    qos_puts(".\n");
}

static int foreground_process_exited(void){
    if (g_foreground_pid < 0){
        return 0;
    }
    int st = qos_process_state(g_foreground_pid);
    if (st > QOS_PROC_DEAD && st != QOS_PROC_REAPING){
        return 0;
    }
    g_foreground_pid = -1;
    (void)qos_display_switch_session(0u);
    qos_puts("\nProcess exited, returned to shell.\n");
    return 1;
}

static void cmd_exit_process(const char* arg){
    unsigned int pid = 0;
    int target = g_foreground_pid;

    if (arg){
        while (*arg == ' '){
            arg++;
        }
        if (*arg){
            if (parse_uint(arg, &pid) != 0){
                qos_puts("Usage: exit [pid]\n");
                return;
            }
            target = (int)pid;
        }
    }

    if (target <= 0){
        qos_puts("No foreground process to exit. Use: exit <pid>\n");
        return;
    }
    if (qos_process_kill(target) != 0){
        qos_puts("Process exit failed.\n");
        return;
    }
    g_foreground_pid = target;
    (void)foreground_process_exited();
}

static void cmd_log(const char* arg){
    unsigned int pid_u = 0u;
    int target = g_foreground_pid;

    if (arg){
        while (*arg == ' '){
            arg++;
        }
        if (*arg){
            if (parse_uint(arg, &pid_u) != 0){
                qos_puts("Usage: log [pid]\n");
                return;
            }
            target = (int)pid_u;
        }
    }

    if (target < 0){
        qos_puts("No foreground process. Use: log <pid>\n");
        return;
    }

    int n = qos_process_log_read(target, g_log_buf, sizeof(g_log_buf));
    if (n < 0){
        qos_puts("Log unavailable.\n");
        return;
    }
    qos_puts("Log PID ");
    print_uint((unsigned int)target);
    qos_puts(":\n");
    if (n == 0){
        qos_puts("(empty)\n");
        return;
    }
    qos_puts(g_log_buf);
    if (g_log_buf[(unsigned int)n - 1u] != '\n'){
        qos_puts("\n");
    }
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

static void cmd_ledtest(const char* mode){
    if (!mode || !*mode || str_eq(mode, "status")){
        unsigned int st = qos_led_status();
        unsigned int enabled = (st & (1u << 0)) ? 1u : 0u;
        unsigned int on = (st & (1u << 1)) ? 1u : 0u;
        unsigned int test = (st & (1u << 2)) ? 1u : 0u;
        unsigned int active_high = (st & (1u << 3)) ? 1u : 0u;
        unsigned int manual = (st >> 4) & 0x3u;
        unsigned int pin = (st >> 8) & 0xFFu;
        unsigned int pin_alt = (st >> 16) & 0xFFu;

        qos_puts("led: enabled=");
        qos_puts(enabled ? "yes" : "no");
        qos_puts(" state=");
        qos_puts(on ? "on" : "off");
        qos_puts(" test=");
        qos_puts(test ? "active" : "idle");
        qos_puts(" pin=");
        print_uint(pin);
        if (pin_alt < 54u){
            qos_puts(" alt=");
            print_uint(pin_alt);
        }
        qos_puts(" mode=");
        if (manual == 1u){
            qos_puts("force-off");
        } else if (manual == 2u){
            qos_puts("force-on");
        } else{
            qos_puts("auto");
        }
        qos_puts(" polarity=");
        qos_puts(active_high ? "active-high" : "active-low");
        qos_puts("\n");
        return;
    }

    if (str_eq(mode, "on")){
        if (qos_led_set(1u) == 0){
            qos_puts("LED forced on.\n");
        } else{
            qos_puts("LED force-on failed.\n");
        }
        return;
    }

    if (str_eq(mode, "off")){
        if (qos_led_set(0u) == 0){
            qos_puts("LED forced off.\n");
        } else{
            qos_puts("LED force-off failed.\n");
        }
        return;
    }

    if (str_eq(mode, "auto")){
        if (qos_led_set(2u) == 0){
            qos_puts("LED auto mode restored.\n");
        } else{
            qos_puts("LED auto restore failed.\n");
        }
        return;
    }

    {
        unsigned int blinks = 0u;
        unsigned int on_ms = 500u;
        unsigned int off_ms = 500u;
        char local[64];
        unsigned int i = 0u;
        const char* p = mode;
        while (p && *p && i + 1u < sizeof(local)){
            local[i++] = *p++;
        }
        local[i] = 0;

        char* q = local;
        while (*q == ' '){
            q++;
        }
        if (*q){
            char* a0 = q;
            while (*q && *q != ' '){
                q++;
            }
            if (*q){
                *q++ = 0;
                while (*q == ' '){
                    q++;
                }
            }

            if (parse_uint(a0, &blinks) != 0){
                qos_puts("Usage: ledtest [blinks [on_ms off_ms]] | on | off | auto | status\n");
                return;
            }

            if (*q){
                char* a1 = q;
                while (*q && *q != ' '){
                    q++;
                }
                if (*q){
                    *q++ = 0;
                    while (*q == ' '){
                        q++;
                    }
                }
                if (parse_uint(a1, &on_ms) != 0){
                    qos_puts("Usage: ledtest [blinks [on_ms off_ms]] | on | off | auto | status\n");
                    return;
                }
                if (*q){
                    char* a2 = q;
                    while (*q && *q != ' '){
                        q++;
                    }
                    if (*q){
                        qos_puts("Usage: ledtest [blinks [on_ms off_ms]] | on | off | auto | status\n");
                        return;
                    }
                    if (parse_uint(a2, &off_ms) != 0){
                        qos_puts("Usage: ledtest [blinks [on_ms off_ms]] | on | off | auto | status\n");
                        return;
                    }
                }
            }
        }

        if (qos_led_test(blinks, on_ms, off_ms) == 0){
            qos_puts("LED test started.\n");
        } else{
            qos_puts("LED test failed.\n");
        }
    }
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
        qos_puts(" xfers=");
        print_uint(qos_dma_transfer_count());
        qos_puts(" last_bytes=");
        print_uint(qos_dma_last_bytes());
        qos_puts(" clean_us=");
        print_uint(qos_dma_last_clean_us());
        qos_puts(" wait_us=");
        print_uint(qos_dma_last_wait_us());
        qos_puts(" total_us=");
        print_uint(qos_dma_last_total_us());
        qos_puts("\n");
        return;
    }
    if (str_eq(mode, "on")){
        (void)qos_dma_set_enabled(1);
        qos_puts("dma enabled for large framebuffer presents.\n");
        return;
    }
    if (str_eq(mode, "off")){
        (void)qos_dma_set_enabled(0);
        qos_puts("dma disabled; framebuffer presents use CPU copy.\n");
        return;
    }
    qos_puts("Usage: dma on|off|status\n");
}

static void cmd_gpu(const char* mode){
    unsigned int st;
    if (!mode || !*mode || str_eq(mode, "status")){
        st = qos_gpu_status();
        qos_puts("gpu=");
        qos_puts((st & 1u) ? "on" : "off");
        qos_puts(" pageflip=");
        qos_puts((st & 2u) ? "yes" : "no");
        qos_puts(" scanout=");
        qos_puts((st & 4u) ? "direct" : "buffered");
        qos_puts(" pages=");
        print_uint((st >> 8) & 0xFFu);
        qos_puts(" failures=");
        print_uint((st >> 16) & 0xFFFFu);
        qos_puts(" flips=");
        print_uint(qos_gpu_flip_count());
        qos_puts("\n");
        return;
    }
    if (str_eq(mode, "on")){
        if (qos_gpu_set_enabled(1) == 0){
            qos_puts("gpu page-flip acceleration enabled.\n");
        } else{
            qos_puts("gpu page-flip unavailable on this framebuffer.\n");
        }
        return;
    }
    if (str_eq(mode, "off")){
        (void)qos_gpu_set_enabled(0);
        qos_puts("gpu acceleration disabled; graphics use buffered CPU present.\n");
        return;
    }
    qos_puts("Usage: gpu on|off|status\n");
}

static void cmd_vsync(const char* mode){
    if (!mode || !*mode || str_eq(mode, "status")){
        qos_puts("vsync=");
        qos_puts(qos_display_vsync_get() ? "on" : "off");
        qos_puts("\n");
        return;
    }
    if (str_eq(mode, "on")){
        (void)qos_display_vsync_set(1);
        qos_puts("vsync enabled; presents are paced to vblank.\n");
        return;
    }
    if (str_eq(mode, "off")){
        (void)qos_display_vsync_set(0);
        qos_puts("vsync disabled for benchmark mode; tearing is expected.\n");
        return;
    }
    qos_puts("Usage: vsync on|off|status\n");
}

static void cmd_gpu2d(const char* mode){
    unsigned int st;
    if (mode && str_eq(mode, "qpu on")){
        (void)qos_gpu2d_qpu_set_enabled(1);
        qos_puts("gpu2d qpu path enabled; unsupported quads still fall back to CPU.\n");
        return;
    }
    if (mode && str_eq(mode, "qpu off")){
        (void)qos_gpu2d_qpu_set_enabled(0);
        qos_puts("gpu2d qpu path disabled.\n");
        return;
    }
    if (mode && str_eq(mode, "v3d on")){
        (void)qos_gpu2d_v3d_set_enabled(1);
        qos_puts("gpu2d v3d fill-batch path enabled for 64px-aligned solid quads.\n");
        return;
    }
    if (mode && str_eq(mode, "v3d off")){
        (void)qos_gpu2d_v3d_set_enabled(0);
        qos_puts("gpu2d v3d fill-batch path disabled.\n");
        return;
    }
    if (mode && *mode && !str_eq(mode, "status") && !str_eq(mode, "qpu") && !str_eq(mode, "v3d")){
        qos_puts("Usage: gpu2d status|qpu on|qpu off|v3d on|v3d off\n");
        return;
    }

    st = qos_gpu2d_status();
    qos_puts("gpu2d status=");
    print_hex32(st);
    qos_puts(" backend=");
    if (st & QOS_GPU2D_STATUS_BACKEND_HW){
        qos_puts("hw");
    } else if (st & QOS_GPU2D_STATUS_BACKEND_SOFT){
        qos_puts("soft");
    } else{
        qos_puts("none");
    }
    qos_puts(" caps=");
    if (st & QOS_GPU2D_CAP_SOFTWARE_FALLBACK) qos_puts("sw ");
    if (st & QOS_GPU2D_CAP_ACCEL_BLIT) qos_puts("blit ");
    if (st & QOS_GPU2D_CAP_ACCEL_SCALE) qos_puts("scale ");
    if (st & QOS_GPU2D_CAP_ACCEL_ALPHA) qos_puts("alpha ");
    if (st & QOS_GPU2D_CAP_ACCEL_ROTATE) qos_puts("rotate ");
    if (st & QOS_GPU2D_CAP_ACCEL_CLEAR) qos_puts("clear ");
    if (st & QOS_GPU2D_CAP_ACCEL_FILL_TILE) qos_puts("filltile ");
    if (st & QOS_GPU2D_CAP_TEXTURE_OBJECTS) qos_puts("textures ");
    if (st & QOS_GPU2D_CAP_QUAD_BATCH) qos_puts("quadbatch ");
    if (st & QOS_GPU2D_CAP_QPU_QUAD) qos_puts("qpuquad ");
    if (st & QOS_GPU2D_CAP_V3D_FILL_BATCH) qos_puts("v3dfillbatch ");
    qos_puts("qpu=");
    qos_puts(qos_gpu2d_qpu_status() ? "on " : "off ");
    qos_puts("v3d=");
    qos_puts(qos_gpu2d_v3d_status() ? "on " : "off ");
    qos_puts("blits=");
    print_uint(qos_gpu2d_blit_count());
    qos_puts(" clears=");
    print_uint(qos_gpu2d_clear_count());
    qos_puts(" fills=");
    print_uint(qos_gpu2d_fill_count());
    qos_puts(" quads=");
    print_uint(qos_gpu2d_quad_count());
    qos_puts(" qbatch=");
    print_uint(qos_gpu2d_quad_batch_count());
    qos_puts(" qpuq=");
    print_uint(qos_gpu2d_qpu_quad_count());
    qos_puts(" qpufail=");
    print_uint(qos_gpu2d_qpu_fail_count());
    qos_puts(" v3dq=");
    print_uint(qos_gpu2d_v3d_quad_count());
    qos_puts(" v3db=");
    print_uint(qos_gpu2d_v3d_batch_count());
    qos_puts(" v3dfail=");
    print_uint(qos_gpu2d_v3d_fail_count());
    qos_puts(" tex=");
    print_uint(qos_gpu2d_texture_count());
    qos_puts(" up=");
    print_uint(qos_gpu2d_texture_upload_count());
    qos_puts(" free=");
    print_uint(qos_gpu2d_texture_free_count());
    qos_puts(" bytes=");
    print_uint(qos_gpu2d_texture_bytes());
    qos_puts(" fallback=");
    print_uint(qos_gpu2d_fallback_count());
    qos_puts(" unsupported=");
    print_uint(qos_gpu2d_unsupported_count());
    qos_puts("\n");
}

static void cmd_v3d(const char* mode){
    qos_v3d_status_t st;
    int rc;

    if (!mode || !*mode || str_eq(mode, "status")){
        rc = qos_v3d_status(&st);
    } else if (str_eq(mode, "probe")){
        rc = qos_v3d_probe(&st);
    } else if (str_starts_with(mode, "noop")){
        unsigned int thread = 1u;
        const char* p = mode + 4;
        while (*p == ' '){
            p++;
        }
        if (*p && parse_uint(p, &thread) != 0){
            qos_puts("Usage: v3d noop [0|1]\n");
            return;
        }
        rc = qos_v3d_noop(thread, &st);
    } else if (str_eq(mode, "qpu")){
        rc = qos_v3d_qpu_probe(&st);
    } else if (str_eq(mode, "qpuwrite")){
        rc = qos_v3d_qpu_write_probe(&st);
    } else if (str_eq(mode, "qpuexec")){
        rc = qos_v3d_qpu_exec_probe(&st);
    } else if (str_starts_with(mode, "clear")){
        unsigned int rgba = 0u;
        const char* p = mode + 5;
        while (*p == ' '){
            p++;
        }
        if (parse_hex32_arg(p, &rgba) != 0){
            qos_puts("Usage: v3d clear <rgba32hex>\n");
            return;
        }
        rc = qos_v3d_clear(rgba, &st);
    } else{
        qos_puts("Usage: v3d probe|status|noop [0|1]|qpu|qpuwrite|qpuexec|clear <rgba32hex>\n");
        return;
    }

    qos_puts("v3d rc=");
    print_int(rc);
    qos_puts(" flags=");
    print_hex32(st.flags);
    qos_puts(" clock=");
    if (st.flags & QOS_V3D_FLAG_CLOCK_OK){
        print_mhz(st.clock_hz);
    } else{
        qos_puts("?");
    }
    qos_puts("\n");

    qos_puts(" ident0=");
    print_hex32(st.ident0);
    qos_puts(" ident1=");
    print_hex32(st.ident1);
    qos_puts(" ident2=");
    print_hex32(st.ident2);
    qos_puts("\n");

    qos_puts(" scratch before=");
    print_hex32(st.scratch_before);
    qos_puts(" after=");
    print_hex32(st.scratch_after);
    qos_puts(" ct0=");
    print_hex32(st.ct0cs);
    qos_puts(" ct1=");
    print_hex32(st.ct1cs);
    qos_puts(" ca0=");
    print_hex32(st.ct0ca);
    qos_puts(" ca1=");
    print_hex32(st.ct1ca);
    qos_puts(" ea0=");
    print_hex32(st.ct0ea);
    qos_puts(" ea1=");
    print_hex32(st.ct1ea);
    qos_puts(" int=");
    print_hex32(st.intctl);
    qos_puts(" err=");
    print_hex32(st.errstat);
    qos_puts("\n");

    qos_puts(" state:");
    if (st.flags & QOS_V3D_FLAG_PROBED) qos_puts(" probed");
    if (st.flags & QOS_V3D_FLAG_CLOCK_OK) qos_puts(" clock");
    if (st.flags & QOS_V3D_FLAG_QPU_OK) qos_puts(" qpu");
    if (st.flags & QOS_V3D_FLAG_QPU_MEM_OK) qos_puts(" qpumem");
    if (st.flags & QOS_V3D_FLAG_QPU_WRITE_OK) qos_puts(" qpuwrite");
    if (st.flags & QOS_V3D_FLAG_QPU_EXEC_OK) qos_puts(" qpuexec");
    if (st.flags & QOS_V3D_FLAG_PRESENT) qos_puts(" present");
    if (st.flags & QOS_V3D_FLAG_IDENT_OK) qos_puts(" ident");
    if (st.flags & QOS_V3D_FLAG_SCRATCH_OK) qos_puts(" scratch");
    qos_puts(" probes=");
    print_uint(st.probe_count);
    qos_puts(" fails=");
    print_uint(st.fail_count);
    qos_puts(" noop=");
    print_uint(st.noop_count);
    qos_puts(" clear=");
    print_uint(st.clear_count);
    qos_puts(" qpuprobe=");
    print_uint(st.qpu_probe_count);
    qos_puts(" qpuwrite=");
    print_uint(st.qpu_write_count);
    qos_puts(" qpuexec=");
    print_uint(st.qpu_exec_count);
    qos_puts(" last=");
    print_int(st.last_error);
    qos_puts("\n");

    qos_puts(" job thread=");
    print_uint(st.last_job_thread);
    qos_puts(" start=");
    print_hex32(st.last_job_start_bus);
    qos_puts(" end=");
    print_hex32(st.last_job_end_bus);
    qos_puts(" color=");
    print_hex32(st.last_clear_color);
    qos_puts(" page=");
    print_uint(st.last_clear_page);
    qos_puts(" tiles=");
    print_uint(st.last_clear_tiles);
    qos_puts("\n");

    qos_puts(" qpu handle=");
    print_hex32(st.last_qpu_handle);
    qos_puts(" bus=");
    print_hex32(st.last_qpu_bus);
    qos_puts(" size=");
    print_uint(st.last_qpu_size);
    qos_puts(" rc=");
    print_int(st.last_qpu_rc);
    qos_puts("\n");

    qos_puts(" qpu arm=");
    print_hex32(st.last_qpu_arm);
    qos_puts(" w0=");
    print_hex32(st.last_qpu_write0);
    qos_puts(" r0=");
    print_hex32(st.last_qpu_read0);
    qos_puts(" w1=");
    print_hex32(st.last_qpu_write1);
    qos_puts(" r1=");
    print_hex32(st.last_qpu_read1);
    qos_puts(" wrc=");
    print_int(st.last_qpu_write_rc);
    qos_puts("\n");

    qos_puts(" qpuexec status=");
    print_hex32(st.last_qpu_exec_status);
    qos_puts(" mis=");
    print_uint(st.last_qpu_exec_mismatch);
    qos_puts(" exp0=");
    print_hex32(st.last_qpu_exec_expected0);
    qos_puts(" out0=");
    print_hex32(st.last_qpu_exec_result0);
    qos_puts(" exp63=");
    print_hex32(st.last_qpu_exec_expected63);
    qos_puts(" out63=");
    print_hex32(st.last_qpu_exec_result63);
    qos_puts(" erc=");
    print_int(st.last_qpu_exec_rc);
    qos_puts("\n");
}

static void cmd_gfxstat(const char* mode){
    if (mode && str_eq(mode, "reset")){
        qos_display_profile_reset();
        qos_puts("graphics profile reset.\n");
        return;
    }
    qos_display_profile_dump();
}

static void cmd_sysstat(void){
    qos_system_status_t st;
    if (qos_system_status(&st) != 0){
        qos_puts("sysstat unavailable.\n");
        return;
    }

    qos_puts("sysstat: temp=");
    if (st.ok_mask & QOS_SYSTEM_STATUS_TEMP_OK){
        print_temp_millic(st.temp_millic);
    } else{
        qos_puts("?");
    }
    qos_puts(" arm=");
    if (st.ok_mask & QOS_SYSTEM_STATUS_ARM_CLOCK_OK){
        print_mhz(st.arm_hz);
    } else{
        qos_puts("?");
    }
    qos_puts(" core=");
    if (st.ok_mask & QOS_SYSTEM_STATUS_CORE_CLOCK_OK){
        print_mhz(st.core_hz);
    } else{
        qos_puts("?");
    }
    qos_puts(" v3d=");
    if (st.ok_mask & QOS_SYSTEM_STATUS_V3D_CLOCK_OK){
        print_mhz(st.v3d_hz);
    } else{
        qos_puts("?");
    }
    qos_puts(" throttled=");
    if (st.ok_mask & QOS_SYSTEM_STATUS_THROTTLE_OK){
        print_hex32(st.throttled_flags);
    } else{
        qos_puts("?");
    }
    qos_puts("\n");

    if ((st.ok_mask & QOS_SYSTEM_STATUS_THROTTLE_OK) && st.throttled_flags){
        qos_puts("throttle flags:");
        if (st.throttled_flags & 0x1u) qos_puts(" under_voltage_now");
        if (st.throttled_flags & 0x2u) qos_puts(" arm_freq_capped_now");
        if (st.throttled_flags & 0x4u) qos_puts(" throttled_now");
        if (st.throttled_flags & 0x8u) qos_puts(" soft_temp_now");
        if (st.throttled_flags & 0x10000u) qos_puts(" under_voltage_seen");
        if (st.throttled_flags & 0x20000u) qos_puts(" arm_freq_capped_seen");
        if (st.throttled_flags & 0x40000u) qos_puts(" throttled_seen");
        if (st.throttled_flags & 0x80000u) qos_puts(" soft_temp_seen");
        qos_puts("\n");
    }
}

static void cmd_clock(const char* arg){
    const char* p = arg;
    while (p && *p == ' '){
        p++;
    }
    if (!p || !*p || str_eq(p, "status")){
        cmd_sysstat();
        return;
    }
    if (str_eq(p, "fast")){
        int ok = 0;
        ok |= qos_system_set_clock(3u, 1200000000u);
        ok |= qos_system_set_clock(4u, 400000000u);
        ok |= qos_system_set_clock(5u, 400000000u);
        if (ok != 0){
            qos_puts("clock fast partially failed.\n");
        } else{
            qos_puts("ARM/core/V3D clocks requested: 1200/400/400 MHz\n");
        }
        cmd_sysstat();
        return;
    }
    if (str_eq(p, "normal")){
        int ok = 0;
        ok |= qos_system_set_clock(3u, 600000000u);
        ok |= qos_system_set_clock(4u, 250000000u);
        ok |= qos_system_set_clock(5u, 250000000u);
        if (ok != 0){
            qos_puts("clock normal partially failed.\n");
        } else{
            qos_puts("ARM/core/V3D clocks requested: normal\n");
        }
        cmd_sysstat();
        return;
    }

    unsigned int clock_id = 0u;
    unsigned int min_mhz = 0u;
    unsigned int max_mhz = 0u;
    if (str_starts_with(p, "arm ")){
        clock_id = 3u;
        min_mhz = 600u;
        max_mhz = 1400u;
        p += 4;
    } else if (str_starts_with(p, "core ")){
        clock_id = 4u;
        min_mhz = 250u;
        max_mhz = 500u;
        p += 5;
    } else if (str_starts_with(p, "v3d ")){
        clock_id = 5u;
        min_mhz = 250u;
        max_mhz = 500u;
        p += 4;
    } else{
        qos_puts("Usage: clock status|fast|normal|arm <mhz>|core <mhz>|v3d <mhz>\n");
        return;
    }
    while (*p == ' '){
        p++;
    }
    unsigned int mhz = 0u;
    if (parse_uint(p, &mhz) != 0 || mhz < min_mhz || mhz > max_mhz){
        qos_puts("Clock MHz out of safe range.\n");
        return;
    }
    if (qos_system_set_clock(clock_id, mhz * 1000000u) != 0){
        qos_puts("clock set failed.\n");
        return;
    }
    qos_puts("Clock requested.\n");
    cmd_sysstat();
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

static void cmd_wifiload(const char* fw83, const char* nv83, const char* clm83){
    const char* fw = (fw83 && *fw83) ? fw83 : g_wifi_fw_83;
    const char* nv = (nv83 && *nv83) ? nv83 : g_wifi_nv_83;
    const char* clm = (clm83 && *clm83) ? clm83 : g_wifi_clm_83;
    int rc = qos_wifi_load_fw(fw, nv, (clm && *clm) ? clm : 0);
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
        qos_puts("WiFi UP failed rc=");
        print_int(rc);
        qos_puts("\n");
    }
}

static void cmd_wifiupmon(void){
    int rc = qos_wifi_up_monitor();
    if (rc == 0){
        qos_puts("WiFi monitor UP OK\n");
    } else{
        qos_puts("WiFi monitor UP failed rc=");
        print_int(rc);
        qos_puts("\n");
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

static void cmd_wifimac(const char* mode){
    if (!mode || !*mode || str_eq(mode, "rand") || str_eq(mode, "random")){
        int rc = qos_wifi_randomize_mac();
        if (rc == 0){
            qos_puts("WiFi MAC randomized (locally-administered).\n");
            qos_wifi_dump_status();
        } else{
            qos_puts("WiFi MAC randomize failed rc=");
            print_int(rc);
            qos_puts("\n");
        }
        return;
    }
    qos_puts("Usage: wifimac rand\n");
}

static void cmd_wifiscan(void){
    cyw43_scan_result_t results[16];
    int n = qos_wifi_scan(results, 16u);
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

static void cmd_wifiscanfor(const char* ssid){
    cyw43_scan_result_t results[16];
    int n;
    if (!ssid || !*ssid){
        qos_puts("Usage: wifiscanfor <ssid>\n");
        return;
    }
    n = qos_wifi_scan_ssid(ssid, results, 16u);
    if (n < 0){
        qos_puts("WiFi directed scan failed\n");
        return;
    }
    qos_puts("WiFi directed scan entries=");
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
        cyw43_scan_result_t batch[16];
        int n = qos_wifi_scan(batch, 16u);
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

static void cmd_wifiraw(const char* mode){
    if (!mode || !*mode || str_eq(mode, "stat")){
        cyw43_raw_capture_status_t st;
        if (qos_wifi_raw_status(&st) != 0){
            qos_puts("WiFi raw status failed\n");
            return;
        }
        qos_puts("wifiraw=");
        qos_puts(st.enabled ? "on" : "off");
        qos_puts(" queued=");
        print_uint(st.queued);
        qos_puts(" rx=");
        print_uint(st.rx_frames);
        qos_puts(" drop=");
        print_uint(st.dropped);
        qos_puts(" trunc=");
        print_uint(st.truncated);
        qos_puts(" last_len=");
        print_uint(st.last_len);
        qos_puts(" kind=");
        print_uint(st.last_kind);
        qos_puts(" rt=");
        print_uint(st.radiotap_frames);
        qos_puts(" dot11=");
        print_uint(st.dot11_frames);
        qos_puts(" eth=");
        print_uint(st.ethernet_frames);
        qos_puts(" unk=");
        print_uint(st.unknown_frames);
        qos_puts("\n");
        return;
    }

    if (str_eq(mode, "on")){
        if (qos_wifi_raw_set_enabled(1u) == 0){
            qos_puts("WiFi raw capture enabled.\n");
        } else{
            qos_puts("WiFi raw capture enable failed.\n");
        }
        return;
    }

    if (str_eq(mode, "off")){
        if (qos_wifi_raw_set_enabled(0u) == 0){
            qos_puts("WiFi raw capture disabled.\n");
        } else{
            qos_puts("WiFi raw capture disable failed.\n");
        }
        return;
    }

    if (str_eq(mode, "read")){
        unsigned char buf[256];
        int n = qos_wifi_raw_recv(buf, sizeof(buf));
        if (n < 0){
            qos_puts("WiFi raw read failed\n");
            return;
        }
        if (n == 0){
            qos_puts("WiFi raw queue empty\n");
            return;
        }
        qos_puts("WiFi raw len=");
        print_uint((unsigned int)n);
        qos_puts(" data=");
        unsigned int show = (n > 64) ? 64u : (unsigned int)n;
        for (unsigned int i = 0; i < show; i++){
            if (i){
                qos_putc(' ');
            }
            print_hex8(buf[i]);
        }
        if ((unsigned int)n > show){
            qos_puts(" ...");
        }
        qos_puts("\n");
        return;
    }

    if (str_starts_with(mode, "drain")){
        unsigned char buf[256];
        const char* p = mode + 5;
        unsigned int ms = 1000u;
        unsigned int frames = 0u;
        unsigned int bytes = 0u;
        unsigned int max_len = 0u;
        unsigned long long start = 0;
        unsigned long long deadline = 0;
        cyw43_raw_capture_status_t st;

        while (*p == ' '){
            p++;
        }
        if (*p && parse_uint(p, &ms) != 0){
            qos_puts("Usage: wifiraw drain [ms]\n");
            return;
        }
        if (ms == 0u){
            ms = 1000u;
        }
        if (ms > 10000u){
            ms = 10000u;
        }

        start = qos_get_time_us();
        deadline = start + ((unsigned long long)ms * 1000ull);
        while ((long long)(qos_get_time_us() - deadline) < 0){
            int n = qos_wifi_raw_recv(buf, sizeof(buf));
            if (n < 0){
                qos_puts("WiFi raw drain failed\n");
                return;
            }
            if (n > 0){
                frames++;
                bytes += (unsigned int)n;
                if ((unsigned int)n > max_len){
                    max_len = (unsigned int)n;
                }
                continue;
            }
            qos_sleep(1);
        }

        if (qos_wifi_raw_status(&st) != 0){
            qos_puts("WiFi raw status failed\n");
            return;
        }
        qos_puts("WiFi raw drain ms=");
        print_uint(ms);
        qos_puts(" frames=");
        print_uint(frames);
        qos_puts(" bytes=");
        print_uint(bytes);
        qos_puts(" max=");
        print_uint(max_len);
        qos_puts(" queued=");
        print_uint(st.queued);
        qos_puts(" rx=");
        print_uint(st.rx_frames);
        qos_puts(" drop=");
        print_uint(st.dropped);
        qos_puts(" rt=");
        print_uint(st.radiotap_frames);
        qos_puts(" dot11=");
        print_uint(st.dot11_frames);
        qos_puts(" eth=");
        print_uint(st.ethernet_frames);
        qos_puts(" unk=");
        print_uint(st.unknown_frames);
        qos_puts("\n");
        return;
    }

    if (str_starts_with(mode, "scan")){
        unsigned char buf[512];
        const char* p = mode + 4;
        unsigned int ms = 1000u;
        unsigned int frames = 0u;
        unsigned int rt = 0u;
        unsigned int dot11 = 0u;
        unsigned int beacon = 0u;
        unsigned int probe_req = 0u;
        unsigned int probe_resp = 0u;
        unsigned int data = 0u;
        unsigned int eapol = 0u;
        unsigned int malformed = 0u;
        unsigned long long start = 0;
        unsigned long long deadline = 0;

        while (*p == ' '){
            p++;
        }
        if (*p && parse_uint(p, &ms) != 0){
            qos_puts("Usage: wifiraw scan [ms]\n");
            return;
        }
        if (ms == 0u){
            ms = 1000u;
        }
        if (ms > 10000u){
            ms = 10000u;
        }

        start = qos_get_time_us();
        deadline = start + ((unsigned long long)ms * 1000ull);
        while ((long long)(qos_get_time_us() - deadline) < 0){
            int n = qos_wifi_raw_recv(buf, sizeof(buf));
            const unsigned char* dot = buf;
            unsigned int dot_len = (n > 0) ? (unsigned int)n : 0u;
            unsigned int fc = 0u;
            unsigned int type = 0u;
            unsigned int subtype = 0u;
            unsigned int hdr_len = 24u;

            if (n < 0){
                qos_puts("WiFi raw scan failed\n");
                return;
            }
            if (n == 0){
                qos_sleep(1);
                continue;
            }
            frames++;

            if (dot_len >= 8u && buf[0] == 0u && buf[1] == 0u){
                unsigned int rt_len = read_le16_shell(buf + 2u);
                if (rt_len >= 8u && rt_len < dot_len){
                    rt++;
                    dot = buf + rt_len;
                    dot_len -= rt_len;
                }
            }

            if (dot_len < 24u){
                malformed++;
                continue;
            }
            fc = read_le16_shell(dot);
            if ((fc & 0x0003u) != 0u){
                malformed++;
                continue;
            }
            type = (fc >> 2) & 0x3u;
            subtype = (fc >> 4) & 0xFu;

            if (type == 0u && subtype == 8u){
                beacon++;
            } else if (type == 0u && subtype == 4u){
                probe_req++;
            } else if (type == 0u && subtype == 5u){
                probe_resp++;
            } else if (type == 2u){
                unsigned int qos = (subtype & 0x8u) ? 1u : 0u;
                unsigned int to_ds = (fc >> 8) & 1u;
                unsigned int from_ds = (fc >> 9) & 1u;
                unsigned int llc = 0u;
                data++;
                if (to_ds && from_ds){
                    hdr_len += 6u;
                }
                if (qos){
                    hdr_len += 2u;
                }
                llc = hdr_len;
                if (dot_len >= llc + 8u &&
                    dot[llc + 0u] == 0xAAu &&
                    dot[llc + 1u] == 0xAAu &&
                    dot[llc + 2u] == 0x03u &&
                    dot[llc + 6u] == 0x88u &&
                    dot[llc + 7u] == 0x8Eu){
                    eapol++;
                }
            }
        }

        qos_puts("WiFi raw scan ms=");
        print_uint(ms);
        qos_puts(" frames=");
        print_uint(frames);
        qos_puts(" rt=");
        print_uint(rt);
        qos_puts(" beacon=");
        print_uint(beacon);
        qos_puts(" probe_req=");
        print_uint(probe_req);
        qos_puts(" probe_resp=");
        print_uint(probe_resp);
        qos_puts(" data=");
        print_uint(data);
        qos_puts(" eapol=");
        print_uint(eapol);
        qos_puts(" bad=");
        print_uint(malformed);
        qos_puts("\n");
        return;
    }

    qos_puts("Usage: wifiraw on|off|stat|read|drain [ms]|scan [ms]\n");
}

static void cmd_wifimon(const char* mode){
    if (!mode || !*mode || str_eq(mode, "status")){
        cyw43_monitor_status_t st;
        if (qos_wifi_monitor_status(&st) != 0){
            qos_puts("WiFi monitor status failed\n");
            return;
        }
        qos_puts("wifimon=");
        qos_puts(st.enabled ? "on" : "off");
        qos_puts(" req=");
        print_uint(st.requested_mode);
        qos_puts(" mon=");
        print_uint(st.monitor);
        qos_puts(" promisc=");
        print_uint(st.promisc);
        qos_puts(" scanstop=");
        print_uint(st.scansuppress);
        qos_puts(" ch=");
        print_uint(st.channel);
        qos_puts(" raw=");
        print_uint(st.raw_enabled);
        qos_puts(" rc=");
        print_int(st.last_rc);
        qos_puts("\n");
        return;
    }

    if (str_eq(mode, "off")){
        int rc = qos_wifi_monitor_set(0u, 0u);
        if (rc == 0){
            qos_puts("WiFi monitor disabled.\n");
        } else{
            qos_puts("WiFi monitor disable failed rc=");
            print_int(rc);
            qos_puts("\n");
        }
        return;
    }

    if (str_starts_with(mode, "on")){
        const char* p = mode + 2;
        unsigned int channel = 0;
        while (*p == ' '){
            p++;
        }
        if (*p && parse_uint(p, &channel) != 0){
            qos_puts("Usage: wifimon on [channel]\n");
            return;
        }
        int rc = qos_wifi_monitor_set(2u, channel);
        if (rc == 0){
            qos_puts("WiFi monitor enabled mode=2");
            if (channel){
                qos_puts(" ch=");
                print_uint(channel);
            }
            qos_puts("\n");
        } else{
            qos_puts("WiFi monitor enable failed rc=");
            print_int(rc);
            qos_puts(" (stock firmware may reject WLC_SET_MONITOR)\n");
        }
        return;
    }

    qos_puts("Usage: wifimon on [channel]|off|status\n");
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
    } else if (str_eq(g_buf, "runbg")){
        cmd_runbg();
    } else if (str_eq(g_buf, "exit")){
        cmd_exit_process(0);
    } else if (str_starts_with(g_buf, "exit ")){
        cmd_exit_process(g_buf + 5);
    } else if (str_eq(g_buf, "log")){
        cmd_log(0);
    } else if (str_starts_with(g_buf, "log ")){
        cmd_log(g_buf + 4);
    } else if (str_starts_with(g_buf, "gfx ")){
        const char* p = g_buf + 4;
        unsigned int pid = 0;
        while (*p == ' '){
            p++;
        }
        if (parse_uint(p, &pid) != 0){
            qos_puts("Usage: gfx <pid>\n");
        } else{
            cmd_gfx(pid);
        }
    } else if (str_eq(g_buf, "gfx")){
        qos_puts("Usage: gfx <pid>\n");
    } else if (str_eq(g_buf, "game")){
        cmd_game();
    } else if (str_eq(g_buf, "scanner")){
        cmd_scanner();
    } else if (str_eq(g_buf, "scanlog")){
        cmd_scanlog("handshakes");
    } else if (str_starts_with(g_buf, "scanlog ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_scanlog(p);
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
    } else if (str_starts_with(g_buf, "ledtest ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_ledtest(p);
    } else if (str_eq(g_buf, "ledtest")){
        cmd_ledtest("6 500 500");
    } else if (str_starts_with(g_buf, "dma ")){
        const char* p = g_buf + 4;
        while (*p == ' '){
            p++;
        }
        cmd_dma(p);
    } else if (str_eq(g_buf, "dma")){
        cmd_dma("status");
    } else if (str_starts_with(g_buf, "gpu ")){
        const char* p = g_buf + 4;
        while (*p == ' '){
            p++;
        }
        cmd_gpu(p);
    } else if (str_eq(g_buf, "gpu")){
        cmd_gpu("status");
    } else if (str_starts_with(g_buf, "vsync ")){
        const char* p = g_buf + 6;
        while (*p == ' '){
            p++;
        }
        cmd_vsync(p);
    } else if (str_eq(g_buf, "vsync")){
        cmd_vsync("status");
    } else if (str_starts_with(g_buf, "gpu2d ")){
        const char* p = g_buf + 6;
        while (*p == ' '){
            p++;
        }
        cmd_gpu2d(p);
    } else if (str_eq(g_buf, "gpu2d")){
        cmd_gpu2d("status");
    } else if (str_starts_with(g_buf, "v3d ")){
        const char* p = g_buf + 4;
        while (*p == ' '){
            p++;
        }
        cmd_v3d(p);
    } else if (str_eq(g_buf, "v3d")){
        cmd_v3d("status");
    } else if (str_starts_with(g_buf, "gfxstat ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_gfxstat(p);
    } else if (str_eq(g_buf, "gfxstat")){
        cmd_gfxstat(0);
    } else if (str_eq(g_buf, "sysstat")){
        cmd_sysstat();
    } else if (str_starts_with(g_buf, "clock ")){
        const char* p = g_buf + 6;
        while (*p == ' '){
            p++;
        }
        cmd_clock(p);
    } else if (str_eq(g_buf, "clock")){
        cmd_clock("status");
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
        char* clm = 0;
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
                    while (*p && *p != ' '){
                        p++;
                    }
                    if (*p){
                        *p++ = 0;
                        while (*p == ' '){
                            p++;
                        }
                        if (*p){
                            clm = p;
                        }
                    }
                }
            }
        }
        cmd_wifiload(fw, nv, clm);
    } else if (str_eq(g_buf, "wifiload")){
        cmd_wifiload(0, 0, 0);
    } else if (str_eq(g_buf, "wifiup")){
        cmd_wifiup();
    } else if (str_eq(g_buf, "wifiupmon")){
        cmd_wifiupmon();
    } else if (str_eq(g_buf, "wifidown")){
        cmd_wifidown();
    } else if (str_eq(g_buf, "wifistat")){
        qos_wifi_dump_status();
    } else if (str_eq(g_buf, "wifiver")){
        cmd_wifiver();
    } else if (str_starts_with(g_buf, "wifimac ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_wifimac(p);
    } else if (str_eq(g_buf, "wifimac")){
        cmd_wifimac("rand");
    } else if (str_eq(g_buf, "wifiscan")){
        cmd_wifiscan();
    } else if (str_starts_with(g_buf, "wifiscanfor ")){
        char* p = g_buf + 12;
        char* ssid = parse_arg_token(&p);
        char* extra = parse_arg_token(&p);
        if (extra && *extra){
            qos_puts("Usage: wifiscanfor <ssid>\n");
            qos_puts("   or: wifiscanfor \"ssid with spaces\"\n");
        } else{
            cmd_wifiscanfor(ssid);
        }
    } else if (str_eq(g_buf, "wifiscanfor")){
        qos_puts("Usage: wifiscanfor <ssid>\n");
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
    } else if (str_starts_with(g_buf, "wifijoinhidden ")){
        char* p = g_buf + 15;
        char* ssid = parse_arg_token(&p);
        char* password = parse_arg_token(&p);
        char* extra = parse_arg_token(&p);
        if (extra && *extra){
            qos_puts("Usage: wifijoinhidden <ssid> <password>\n");
            qos_puts("   or: wifijoinhidden \"ssid with spaces\" \"password with spaces\"\n");
        } else{
            cmd_wifijoin(ssid, password);
        }
    } else if (str_eq(g_buf, "wifijoinhidden")){
        qos_puts("Usage: wifijoinhidden <ssid> <password>\n");
        qos_puts("   or: wifijoinhidden \"ssid with spaces\" \"password with spaces\"\n");
    } else if (str_starts_with(g_buf, "wifiraw ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_wifiraw(p);
    } else if (str_eq(g_buf, "wifiraw")){
        cmd_wifiraw("stat");
    } else if (str_starts_with(g_buf, "wifimon ")){
        const char* p = g_buf + 8;
        while (*p == ' '){
            p++;
        }
        cmd_wifimon(p);
    } else if (str_eq(g_buf, "wifimon")){
        cmd_wifimon("status");
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
        if (shell_can_use_input_overlay()){
            (void)qos_term_clear_input_line();
            qos_puts("UQOS> ");
            qos_puts(src_buf);
            qos_puts("\n");
        } else{
            qos_puts("\n");
        }
        execute_source_buffer(src_buf, src_len);
        print_prompt();
        return;
    }

    if (ch == 127 || ch == '\b'){
        if (*src_len > 0){
            (*src_len)--;
            src_buf[*src_len] = 0;
            if (shell_can_use_input_overlay()){
                if (shell_set_input_overlay_from(src_buf, *src_len) != 0){
                    qos_puts("\b \b");
                }
            } else{
                qos_puts("\b \b");
            }
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
        if (shell_can_use_input_overlay()){
            if (shell_set_input_overlay_from(src_buf, *src_len) != 0){
                qos_putc((char)ch);
            }
        } else{
            qos_putc((char)ch);
        }
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
            g_foreground_pid = -1;
            qos_puts("\nProcess exited, returned to shell.\n");
            print_prompt();
        } else if (!shell_has_tty){
            g_tty_owned = 0;
            // Avoid tight spin during ownership handoff races.
            qos_sleep(1);
            continue;
        }
        if (foreground_process_exited()){
            print_prompt();
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
