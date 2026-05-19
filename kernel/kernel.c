#include "uart.h"
#include "memory.h"
//#include "task.h"
#include "framebuffer.h"
#include "context.h"
#include "api.h"
#include "loader.h"
#include "process.h"
#include "fat32.h"
//#include "sd.h"
#include "gpio.h"
#include "blockdev.h"
//#include "clock.h"
#include "mailbox.h"
#include "debug.h"
#include "interrupt.h"
#include "mmu.h"
#include "net.h"
#include "net_proto.h"
#include "arp.h"
#include "usb_host.h"
#include "socket.h"
#include "console.h"
#include "cpu.h"
#include "smp.h"
#include "kernel_verify.h"
#include "crypto.h"
#include "aes_gcm.h"
#include "tls_record.h"
#include "x25519.h"
#include "pq_kem.h"
#include "tls_key_schedule.h"
#include "tls_handshake.h"
#include "tls_session.h"
#include "pq_sig.h"
#include "auth.h"
#include "remote_login.h"
#include "cyw43.h"
#include "panic.h"
#include "terminal.h"
#include "klog.h"
#include "display.h"
#include "program.h"
#include "headless_control.h"
#include "sandbox_file.h"
#include "platform/board.h"
#include "platform/soc.h"
#include "timer.h"

static int kernel_ranges_overlap(unsigned long a, unsigned long a_size,
                                 unsigned long b, unsigned long b_size){
    if (a_size == 0UL || b_size == 0UL){
        return 0;
    }
    unsigned long a_end = a + a_size;
    unsigned long b_end = b + b_size;
    if (a_end < a || b_end < b){
        return 1;
    }
    return a < b_end && b < a_end;
}


//extern kernel_api_t kapi;

//static unsigned char sector[512];

void memzero(unsigned long start, unsigned long size){
    for (unsigned long i = 0; i < size; i++)
        ((char*)start)[i] = 0;
}

#if defined(QOS_BOARD_PI_ZERO2W) && QOS_BOARD_PI_ZERO2W
#define PI0_AUTO_WIFI_FW_83  "P0WIFI36BIN"
#define PI0_AUTO_WIFI_NV_83  "P0WIFI36TXT"
#define PI0_AUTO_WIFI_CLM_83 "P0WIFI36CLM"

static void pi0_wifi_wait_ms(unsigned int ms){
    unsigned long start = system_ticks;
    if (ms == 0u){
        return;
    }
    while ((unsigned long)(system_ticks - start) < (unsigned long)ms){
        asm volatile("wfe" : : : "memory");
    }
}

static int pi0_wait_for_gateway_arp(unsigned int timeout_ms){
    unsigned char gateway_ip[4];
    unsigned long start = system_ticks;
    unsigned long next_req = start;

    if (arp_gateway_resolved()){
        return 0;
    }
    net_proto_get_gateway_ip(gateway_ip);
    if (timeout_ms == 0u){
        timeout_ms = 2500u;
    }

    while ((unsigned long)(system_ticks - start) < (unsigned long)timeout_ms){
        if (arp_gateway_resolved()){
            return 0;
        }
        if ((long)(system_ticks - next_req) >= 0){
            (void)arp_send_request(gateway_ip);
            next_req = system_ticks + 250u;
        }
        (void)net_poll();
        if (arp_gateway_resolved()){
            return 0;
        }
        pi0_wifi_wait_ms(10u);
    }
    return arp_gateway_resolved() ? 0 : -1;
}

static void pi0_headless_write(const char* s){
    unsigned long len = 0;
    if (!s){
        return;
    }
    while (s[len]){
        len++;
    }
    terminal_write(0, -1, s, len);
}

static void pi0_headless_putdec(unsigned long v){
    char tmp[21];
    char out[21];
    int n = 0;
    int o = 0;

    if (v == 0UL){
        pi0_headless_write("0");
        return;
    }
    while (v > 0UL && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0 && o + 1 < (int)sizeof(out)){
        out[o++] = tmp[--n];
    }
    out[o] = 0;
    pi0_headless_write(out);
}

static void pi0_headless_remote_diag(void){
    unsigned long rlogin_rx = 0;
    unsigned long rlogin_tx = 0;
    unsigned long rlogin_bad_hdr = 0;
    unsigned long rlogin_bad_crypto = 0;
    unsigned long net_rx = 0;
    unsigned long net_drop = 0;
    unsigned long net_tx = 0;
    unsigned long net_txfail = 0;
    unsigned int net_rxq = 0;
    const char* driver = "none";
    int link = 0;
    unsigned long arp_rx = 0;
    unsigned long arp_tx_req = 0;
    unsigned long arp_tx_rep = 0;
    unsigned int arp_cache = 0;
    int arp_gw = 0;
    unsigned int rstate = remote_login_state_bits();

    remote_login_get_diag(&rlogin_rx, &rlogin_tx, &rlogin_bad_hdr, &rlogin_bad_crypto);
    net_get_diag(&net_rx, &net_drop, &net_tx, &net_txfail, &net_rxq, &driver, &link);
    arp_get_diag(&arp_rx, &arp_tx_req, &arp_tx_rep, &arp_cache, &arp_gw);

    pi0_headless_write("Pi0 diag: rstate=");
    pi0_headless_putdec(rstate);
    pi0_headless_write(" rrx=");
    pi0_headless_putdec(rlogin_rx);
    pi0_headless_write(" rtx=");
    pi0_headless_putdec(rlogin_tx);
    pi0_headless_write(" bh=");
    pi0_headless_putdec(rlogin_bad_hdr);
    pi0_headless_write(" bc=");
    pi0_headless_putdec(rlogin_bad_crypto);
    pi0_headless_write("\n");

    pi0_headless_write("Pi0 net: drv=");
    pi0_headless_write(driver ? driver : "none");
    pi0_headless_write(" link=");
    pi0_headless_putdec((unsigned long)(link ? 1 : 0));
    pi0_headless_write(" rx=");
    pi0_headless_putdec(net_rx);
    pi0_headless_write(" drop=");
    pi0_headless_putdec(net_drop);
    pi0_headless_write(" tx=");
    pi0_headless_putdec(net_tx);
    pi0_headless_write(" txf=");
    pi0_headless_putdec(net_txfail);
    pi0_headless_write(" rxq=");
    pi0_headless_putdec(net_rxq);
    pi0_headless_write("\n");

    pi0_headless_write("Pi0 arp: rx=");
    pi0_headless_putdec(arp_rx);
    pi0_headless_write(" txreq=");
    pi0_headless_putdec(arp_tx_req);
    pi0_headless_write(" txrep=");
    pi0_headless_putdec(arp_tx_rep);
    pi0_headless_write(" cache=");
    pi0_headless_putdec(arp_cache);
    pi0_headless_write(" gw=");
    pi0_headless_putdec((unsigned long)(arp_gw ? 1 : 0));
    pi0_headless_write("\n");
}

static int cfg_is_space(char c){
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char cfg_lower(char c){
    if (c >= 'A' && c <= 'Z'){
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static int cfg_key_eq(const char* a, const char* b){
    while (*a && *b){
        if (cfg_lower(*a) != cfg_lower(*b)){
            return 0;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static char* cfg_trim(char* s){
    char* e;
    while (*s && cfg_is_space(*s)){
        s++;
    }
    e = s;
    while (*e){
        e++;
    }
    while (e > s && cfg_is_space(e[-1])){
        e--;
        *e = 0;
    }
    return s;
}

static void cfg_copy_value(char* dst, unsigned int cap, const char* src){
    unsigned int i = 0;
    char quote = 0;
    if (!dst || cap == 0){
        return;
    }
    dst[0] = 0;
    if (!src){
        return;
    }
    if (*src == '"' || *src == '\''){
        quote = *src;
        src++;
    }
    while (*src && i + 1u < cap){
        if (quote && *src == quote){
            break;
        }
        dst[i++] = *src++;
    }
    dst[i] = 0;
}

static char cfg_upper(char c){
    if (c >= 'a' && c <= 'z'){
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int cfg_value_is_none(const char* s){
    return cfg_key_eq(s, "none") ||
           cfg_key_eq(s, "off") ||
           cfg_key_eq(s, "skip") ||
           (s[0] == '-' && s[1] == 0);
}

static int cfg_fat83_from_value(char out[12], const char* src){
    char base[8];
    char ext[3];
    char token[24];
    unsigned int bi = 0u;
    unsigned int ei = 0u;
    unsigned int saw_dot = 0u;
    unsigned int len = 0u;
    char quote = 0;

    if (!out || !src){
        return -1;
    }
    for (unsigned int i = 0u; i < 11u; i++){
        out[i] = ' ';
    }
    out[11] = 0;

    while (*src && cfg_is_space(*src)){
        src++;
    }
    if (*src == '"' || *src == '\''){
        quote = *src++;
    }
    while (*src){
        char c = *src++;
        if (quote && c == quote){
            break;
        }
        if (!quote && cfg_is_space(c)){
            const char* rest = src;
            while (*rest){
                if (!cfg_is_space(*rest++)){
                    return -1;
                }
            }
            break;
        }
        if (len + 1u >= sizeof(token)){
            return -1;
        }
        token[len++] = c;
    }
    token[len] = 0;

    if (!token[0] || cfg_value_is_none(token)){
        out[0] = 0;
        return 0;
    }

    if (quote){
        while (*src){
            if (!cfg_is_space(*src++)){
                return -1;
            }
        }
    }

    for (unsigned int i = 0u; i < len; i++){
        if (token[i] == '.'){
            saw_dot = 1u;
            break;
        }
    }
    if (!saw_dot && len == 11u){
        for (unsigned int i = 0u; i < 11u; i++){
            char c = token[i];
            if (c == '/' || c == '\\' || c == ':' ||
                (unsigned char)c < 33u || (unsigned char)c > 126u){
                return -1;
            }
            out[i] = cfg_upper(c);
        }
        return 0;
    }

    for (unsigned int i = 0u; i < 8u; i++) base[i] = ' ';
    for (unsigned int i = 0u; i < 3u; i++) ext[i] = ' ';
    saw_dot = 0u;

    for (const char* p = token; *p; p++){
        char c = *p;
        if (cfg_is_space(c)){
            return -1;
        }
        if (c == '.'){
            if (saw_dot){
                return -1;
            }
            saw_dot = 1u;
            continue;
        }
        if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 33u || (unsigned char)c > 126u){
            return -1;
        }
        c = cfg_upper(c);
        if (saw_dot){
            if (ei >= 3u){
                return -1;
            }
            ext[ei++] = c;
        } else{
            if (bi >= 8u){
                return -1;
            }
            base[bi++] = c;
        }
    }

    if (bi == 0u){
        return -1;
    }

    for (unsigned int i = 0u; i < 8u; i++){
        out[i] = base[i];
    }
    for (unsigned int i = 0u; i < 3u; i++){
        out[8u + i] = ext[i];
    }
    return 0;
}

static int pi0_read_wifi_cfg(char* ssid, unsigned int ssid_cap,
                             char* password, unsigned int password_cap,
                             char fw83[12], char nv83[12], char clm83[12]){
    static char cfg[1024];
    static const char wifi_cfg_83[] = "WIFI    CFG";
    int n;
    char* p;

    if (!ssid || ssid_cap == 0 || !password || password_cap == 0 ||
        !fw83 || !nv83 || !clm83){
        return -1;
    }
    ssid[0] = 0;
    password[0] = 0;
    for (unsigned int i = 0u; i < 12u; i++){
        fw83[i] = 0;
        nv83[i] = 0;
        clm83[i] = 0;
    }
    (void)cfg_fat83_from_value(fw83, PI0_AUTO_WIFI_FW_83);
    (void)cfg_fat83_from_value(nv83, PI0_AUTO_WIFI_NV_83);
    (void)cfg_fat83_from_value(clm83, PI0_AUTO_WIFI_CLM_83);

    n = fat32_read_file(wifi_cfg_83, (unsigned char*)cfg, (int)sizeof(cfg) - 1);
    if (n <= 0){
        return -1;
    }
    cfg[n] = 0;

    p = cfg;
    while (*p){
        char* line = p;
        char* eq;
        char* key;
        char* val;
        while (*p && *p != '\n'){
            p++;
        }
        if (*p == '\n'){
            *p++ = 0;
        }

        line = cfg_trim(line);
        if (!*line || *line == '#'){
            continue;
        }
        eq = line;
        while (*eq && *eq != '='){
            eq++;
        }
        if (*eq != '='){
            continue;
        }
        *eq = 0;
        key = cfg_trim(line);
        val = cfg_trim(eq + 1);

        if (cfg_key_eq(key, "ssid") || cfg_key_eq(key, "wifi_ssid")){
            cfg_copy_value(ssid, ssid_cap, val);
        } else if (cfg_key_eq(key, "password") ||
                   cfg_key_eq(key, "pass") ||
                   cfg_key_eq(key, "psk") ||
                   cfg_key_eq(key, "wifi_password")){
            cfg_copy_value(password, password_cap, val);
        } else if (cfg_key_eq(key, "fw") ||
                   cfg_key_eq(key, "firmware") ||
                   cfg_key_eq(key, "bin") ||
                   cfg_key_eq(key, "wifi_fw")){
            if (cfg_fat83_from_value(fw83, val) != 0 || !fw83[0]){
                memzero((unsigned long)cfg, sizeof(cfg));
                return -1;
            }
        } else if (cfg_key_eq(key, "nvram") ||
                   cfg_key_eq(key, "nv") ||
                   cfg_key_eq(key, "txt") ||
                   cfg_key_eq(key, "wifi_nvram")){
            if (cfg_fat83_from_value(nv83, val) != 0 || !nv83[0]){
                memzero((unsigned long)cfg, sizeof(cfg));
                return -1;
            }
        } else if (cfg_key_eq(key, "clm") ||
                   cfg_key_eq(key, "clm_blob") ||
                   cfg_key_eq(key, "wifi_clm")){
            if (cfg_fat83_from_value(clm83, val) != 0){
                memzero((unsigned long)cfg, sizeof(cfg));
                return -1;
            }
        }
    }

    memzero((unsigned long)cfg, sizeof(cfg));
    return ssid[0] ? 0 : -1;
}

static void pi0_headless_wifi_autojoin(void){
    char ssid[33];
    char password[96];
    char fw83[12];
    char nv83[12];
    char clm83[12];
    int rc;
    int up_ok = 0;
    int join_ok = 0;
    int arp_ok = 0;

    pi0_headless_write("Pi0 headless WiFi: autojoin enabled\n");
    memzero((unsigned long)ssid, sizeof(ssid));
    memzero((unsigned long)password, sizeof(password));
    memzero((unsigned long)fw83, sizeof(fw83));
    memzero((unsigned long)nv83, sizeof(nv83));
    memzero((unsigned long)clm83, sizeof(clm83));

    headless_control_note_boot_stage(4u, 0u);
    if (pi0_read_wifi_cfg(ssid, sizeof(ssid), password, sizeof(password), fw83, nv83, clm83) != 0){
        pi0_headless_write("Pi0 headless WiFi: WIFI.CFG missing/invalid; skipped.\n");
        headless_control_note_boot_stage(4u, 1u);
        return;
    }

    headless_control_note_boot_stage(5u, 0u);
    pi0_headless_write("Pi0 headless WiFi: loading station firmware ");
    pi0_headless_write(fw83);
    pi0_headless_write(" / ");
    pi0_headless_write(nv83);
    pi0_headless_write(" / ");
    pi0_headless_write(clm83[0] ? clm83 : "<no-clm>");
    pi0_headless_write("...\n");
    rc = cyw43_upload_firmware_from_fat(fw83, nv83, clm83);
    if (rc != 0){
        pi0_headless_write("Pi0 headless WiFi: firmware load failed rc=");
        pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
        pi0_headless_write("\n");
        headless_control_note_boot_stage(5u, 1u);
        goto out;
    }

    headless_control_note_boot_stage(6u, 0u);
    pi0_headless_write("Pi0 headless WiFi: raising interface...\n");
    for (unsigned int attempt = 0u; attempt < 3u; attempt++){
        rc = cyw43_ioctl_up();
        if (rc == 0){
            up_ok = 1;
            break;
        }
        pi0_headless_write("Pi0 headless WiFi: wifiup failed attempt=");
        pi0_headless_putdec(attempt + 1u);
        pi0_headless_write(" rc=");
        pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
        pi0_headless_write("\n");
        pi0_wifi_wait_ms(300u + (attempt * 250u));
    }
    if (!up_ok){
        pi0_headless_write("Pi0 headless WiFi: restaging firmware after wifiup failure...\n");
        rc = cyw43_upload_firmware_from_fat(fw83, nv83, clm83);
        if (rc == 0){
            for (unsigned int attempt = 0u; attempt < 3u; attempt++){
                rc = cyw43_ioctl_up();
                if (rc == 0){
                    up_ok = 1;
                    break;
                }
                pi0_headless_write("Pi0 headless WiFi: wifiup restage failed attempt=");
                pi0_headless_putdec(attempt + 1u);
                pi0_headless_write(" rc=");
                pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
                pi0_headless_write("\n");
                pi0_wifi_wait_ms(450u + (attempt * 300u));
            }
        } else{
            pi0_headless_write("Pi0 headless WiFi: firmware restage failed rc=");
            pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
            pi0_headless_write("\n");
        }
    }
    if (!up_ok){
        headless_control_note_boot_stage(6u, 1u);
        goto out;
    }

    headless_control_note_boot_stage(7u, 0u);
    pi0_headless_write("Pi0 headless WiFi: joining hidden SSID...\n");
    for (unsigned int attempt = 0u; attempt < 5u; attempt++){
        rc = cyw43_ioctl_join(ssid, password);
        if (rc == 0){
            join_ok = 1;
            break;
        }
        pi0_headless_write("Pi0 headless WiFi: join failed attempt=");
        pi0_headless_putdec(attempt + 1u);
        pi0_headless_write(" rc=");
        pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
        pi0_headless_write("\n");
        /*
         * Nexmon station joins can be timing-sensitive on Pi0. Do not reload
         * firmware here; just reset the interface state and retry association.
         */
        (void)cyw43_ioctl_down();
        pi0_wifi_wait_ms(250u + (attempt * 150u));
        rc = cyw43_ioctl_up();
        if (rc != 0){
            pi0_headless_write("Pi0 headless WiFi: retry wifiup failed rc=");
            pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
            pi0_headless_write("\n");
            pi0_wifi_wait_ms(500u);
        } else{
            pi0_wifi_wait_ms(350u + (attempt * 150u));
        }
    }
    if (!join_ok){
        headless_control_note_boot_stage(7u, 1u);
        goto out;
    }

    pi0_headless_write("Pi0 headless WiFi: joined; proving RX with gateway ARP...\n");
    if (pi0_wait_for_gateway_arp(3000u) == 0){
        arp_ok = 1;
    }

    /*
     * A successful SET_SSID is not enough for headless boot: some CYW43
     * firmwares return success before the data path is really associated.
     * Gateway ARP is our cheap proof that TX and RX both work.
     */
    for (unsigned int attempt = 0u; !arp_ok && attempt < 3u; attempt++){
        pi0_headless_write("Pi0 headless WiFi: no ARP proof; retrying association ");
        pi0_headless_putdec(attempt + 1u);
        pi0_headless_write("\n");
        (void)cyw43_ioctl_down();
        pi0_wifi_wait_ms(350u + (attempt * 250u));
        rc = cyw43_ioctl_up();
        if (rc != 0){
            pi0_headless_write("Pi0 headless WiFi: ARP retry wifiup failed rc=");
            pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
            pi0_headless_write("\n");
            continue;
        }
        pi0_wifi_wait_ms(350u + (attempt * 150u));
        rc = cyw43_ioctl_join(ssid, password);
        if (rc != 0){
            pi0_headless_write("Pi0 headless WiFi: ARP retry join failed rc=");
            pi0_headless_putdec((unsigned long)(rc < 0 ? -rc : rc));
            pi0_headless_write("\n");
            continue;
        }
        if (pi0_wait_for_gateway_arp(4000u) == 0){
            arp_ok = 1;
        }
    }

    if (arp_ok){
        pi0_headless_write("Pi0 headless WiFi: gateway ARP ready.\n");
    } else{
        pi0_headless_write("Pi0 headless WiFi: gateway ARP pending; remote RX/TX may not work yet.\n");
        pi0_headless_write("Pi0 headless WiFi: direct LAN replies still enabled.\n");
    }
    headless_control_note_wifi_joined_waiting_login();

out:
    memzero((unsigned long)ssid, sizeof(ssid));
    memzero((unsigned long)password, sizeof(password));
    memzero((unsigned long)fw83, sizeof(fw83));
    memzero((unsigned long)nv83, sizeof(nv83));
    memzero((unsigned long)clm83, sizeof(clm83));
}
#else
static void pi0_headless_wifi_autojoin(void){
    uart_puts("Pi0 headless WiFi: skipped; not a BOARD=pi_zero2w build.\n");
}
#endif

void test_task(void *arg){
    char id = (char)(unsigned long)arg;

    uart_puts("Task ");
    uart_send(id);
    uart_puts(" running\n");
}

static loaded_program_t load_boot_shell_program(void){
    return load_program_from_sd_named("SHELL   BIN");
}

static int start_boot_shell_process(loaded_program_t shell_prog){
    if (shell_prog.entry){
        int pid = process_create_loaded(shell_prog);
        if (pid >= 0){
            (void)terminal_attach_pid(pid, 0);
            (void)terminal_set_foreground_pid(0, pid);
            (void)console_set_owner(pid, pid);
            (void)display_set_text_owner(pid);
            (void)display_set_active(DISPLAY_TEXT_SESSION_ID);
            klog_puts("User shell started.\n");
            return pid;
        }

        if (shell_prog.heap_allocated){
            kfree_secure(shell_prog.memory, shell_prog.size);
        } else{
            loader_free_program_memory(shell_prog.memory, shell_prog.size);
        }
        klog_puts("User shell create failed; boot shell disabled by policy.\n");
    } else{
        klog_puts("SHELL.BIN not found; boot shell disabled by policy.\n");
    }

    return -1;
}

static void kernel_poll_background_io(void){
    static unsigned long next_net_poll_tick = 0;
    static unsigned long next_remote_poll_tick = 0;
    static unsigned long next_remote_diag_tick = 0;
    unsigned long now = system_ticks;

    if ((long)(now - next_net_poll_tick) >= 0){
        next_net_poll_tick = now + 10u;
        (void)net_poll();
    }
    if ((long)(now - next_remote_poll_tick) >= 0){
        next_remote_poll_tick = now + 10u;
        remote_login_poll();
    }
#if defined(QOS_BOARD_PI_ZERO2W) && QOS_BOARD_PI_ZERO2W
    if ((long)(now - next_remote_diag_tick) >= 0){
        next_remote_diag_tick = now + 5000u;
        unsigned int st = remote_login_state_bits();
        if ((st & 1u) && !(st & (1u << 2))){
            pi0_headless_remote_diag();
        }
    }
#endif
}

extern unsigned long stack_bottom;

void kernel_secondary_main(void){
    // Per-core EL1 init path for cores 1..3.
    asm volatile(
        "mrs x0, cpacr_el1\n"
        "orr x0, x0, #(3 << 20)\n"
        "msr cpacr_el1, x0\n"
        "isb\n"
        :
        :
        : "x0");

    mmu_enable_secondary();
    interrupt_init();
    smp_mark_core_online(cpu_get_id());
    enable_interrupts();

    while (1){
        asm volatile("wfi");
    }
}

void kernel_main(void){
    // Enable FP/ASIMD at EL1 to avoid EC=0x07 traps on generated code paths.
    asm volatile(
        "mrs x0, cpacr_el1\n"
        "orr x0, x0, #(3 << 20)\n"
        "msr cpacr_el1, x0\n"
        "isb\n"
        :
        :
        : "x0");

    uart_init();
    qos_stack_canary_init();
    uart_puts("Uart initialized!\n");
    uart_puts("Board: ");
    uart_puts(board_name());
    uart_puts(" SoC: ");
    uart_puts(soc_name());
    uart_puts("\n");
    headless_control_init();
#if defined(QOS_BOARD_PI_ZERO2W) && QOS_BOARD_PI_ZERO2W
    uart_puts("Pi0 headless WiFi: build hook present; autojoin after shell load.\n");
#else
    uart_puts("Pi0 headless WiFi: build hook absent for this board.\n");
#endif

    (void)board_power_on_usb();

    mmu_init();
    mailbox_enable_runtime_safety();
    uart_puts("MMU (phase 1 identity map) enabled.\n");
    loader_mmu_init_pool();

    uart_puts("STACK GUARD INIT = ");
    uart_puthex(*(unsigned long*)&stack_bottom);
    uart_puts("\n");
//    check_stack();

    memory_init();
    uart_puts("Memory initialized!\n");
    check_stack();
    crypto_init();
    uart_puts("Crypto initialized!\n");
    if (aes_gcm_self_test() == 0){
        uart_puts("AES-GCM self-test OK\n");
    } else{
        uart_puts("AES-GCM self-test FAILED\n");
    }
    if (tls13_record_self_test() == 0){
        uart_puts("TLS record self-test OK\n");
    } else{
        uart_puts("TLS record self-test FAILED\n");
    }
    int x25519_rc = x25519_self_test();
    if (x25519_rc == 0){
        uart_puts("X25519 self-test OK\n");
    } else{
        unsigned long x25519_abs = (x25519_rc < 0) ? (unsigned long)(-x25519_rc) : (unsigned long)x25519_rc;
        uart_puts("X25519 self-test FAILED rc=");
        uart_putdec(x25519_abs);
        uart_puts("\n");
    }
    if (pq_kem_mlkem768_available()){
        if (pq_kem_mlkem768_self_test() == 0){
            uart_puts("ML-KEM-768 self-test OK\n");
        } else{
            uart_puts("ML-KEM-768 self-test FAILED\n");
        }
    } else{
        uart_puts("ML-KEM-768 backend unavailable (X25519 fallback)\n");
    }
    if (pq_sig_self_test() == 0){
        uart_puts("PQ signature self-test OK\n");
    } else{
        uart_puts("PQ signature self-test FAILED\n");
    }
    if (tls13_key_schedule_self_test() == 0){
        uart_puts("TLS key schedule self-test OK\n");
    } else{
        uart_puts("TLS key schedule self-test FAILED\n");
    }
    if (tls13_handshake_self_test() == 0){
        uart_puts("TLS handshake scaffold self-test OK\n");
    } else{
        uart_puts("TLS handshake scaffold self-test FAILED\n");
    }

    process_init();
    sandbox_file_init();
    interrupt_init();
    enable_interrupts();
    uart_puts("Interrupt system initialized!\n");
    headless_control_init();
    headless_control_note_boot_stage(1u, 0u);

    if (board_has_dwc2_usb()){
        if (usb_host_init() != 0){
            uart_puts("USB host init failed.\n");
        } else{
            if (usb_host_enumerate_root_device() != 0){
                uart_puts("USB: root enumeration failed.\n");
            }
        }
    } else{
        uart_puts("USB DWC2 host not present on this board target.\n");
    }

    if (net_init() != 0){
        uart_puts("NET init failed (continuing without NIC).\n");
    }
    socket_layer_init();
    tls_session_layer_init();
    {
        int tsrc = tls_session_self_test();
        if (tsrc == 0){
            uart_puts("TLS session self-test OK\n");
        } else{
            unsigned long abs = (tsrc < 0) ? (unsigned long)(-tsrc) : (unsigned long)tsrc;
            uart_puts("TLS session self-test FAILED rc=");
            uart_putdec(abs);
            uart_puts("\n");
        }
    }
    console_init();

    fb_init();
    {
        unsigned long fb_base = fb_get_base();
        unsigned long fb_one_page = (unsigned long)fb_get_pitch() * (unsigned long)fb_get_height();
        unsigned long fb_map_size = fb_get_total_size();
        if (fb_map_size == 0UL){
            fb_map_size = fb_one_page;
        }
        if (kernel_ranges_overlap(fb_base,
                                  fb_map_size,
                                  QOS_PROGRAM_POOL_START,
                                  QOS_PROGRAM_POOL_SIZE)){
            uart_puts("FB pageflip disabled: framebuffer overlaps program pool.\n");
            fb_map_size = fb_one_page;
        }
        mmu_map_device_region(fb_base, fb_map_size);
    }
    fb_init_buffers();
    uart_puts("Frame buffer initialized!\n");

//    kapi.clear(0x00000000);
    fb_clear(0x00000000);
    display_init();
    terminal_init();
    klog_set_terminal_ready(1);
//    kapi.draw_rect(100, 100, 500, 300, 0x00FFFFFF);

    klog_puts("Kernel booted successfully!\n");

    smp_mark_core_online(cpu_get_id());
    smp_release_secondary_cores();
    uart_puts("SMP: released cores 1-3\n");
    for (unsigned int i = 0; i < 2000000u; i++){
        if (smp_online_mask() == 0x0Fu){
            break;
        }
        asm volatile("nop");
    }
    uart_puts("SMP: online mask=");
    uart_puthex(smp_online_mask());
    uart_puts("\n");

    // -----------------------------
    // OPTION 1: RUN SHELL (RECOMMENDED)
    // -----------------------------

//    program_entry_t prog;
//    prog = load_program_from_sd();

//    if (prog){
//        void *stack = alloc_stack();
//        if (!stack){
//            uart_puts("No stack available!\n");
//            return;
//        }
//        run_program(prog, stack, &kapi);
//        free_stack(stack);
//    }

//    uart_puts("\n--- START SD PIPELINE ---\n");
//    extern void sdhost_reset(void);
//    extern int sdhost_cmd(unsigned int cmd, unsigned int arg);
//    extern int sdhost_init_card(void);
//    extern int sdhost_read_block(unsigned int lba, unsigned char* buffer);
    if (blockdev_init() != 0){
        klog_puts("Blockdev init failed!\n");
        headless_control_note_boot_stage(1u, 1u);
        while (1){ asm volatile("wfi"); }
    }
    klog_puts("Storage init OK...\n");

    headless_control_note_boot_stage(2u, 0u);
    int kv = kernel_verify_self();
    if (kv < 0){
        klog_puts("Kernel verify failed.\n");
        if (kernel_verify_enforce()){
            klog_puts("Kernel verify enforced: HALTING.\n");
            headless_control_note_boot_stage(2u, 1u);
            while (1){ asm volatile("wfi"); }
        }
        klog_puts("Kernel verify warn-only mode: continuing boot.\n");
    } else if (kv > 0){
        klog_puts("Kernel verify not provisioned yet.\n");
    }

    int kv_file = kernel_verify_storage_image();
    if (kv_file < 0){
        klog_puts("Kernel file verify failed.\n");
        if (kernel_verify_enforce()){
            klog_puts("Kernel verify enforced: HALTING.\n");
            headless_control_note_boot_stage(2u, 1u);
            while (1){ asm volatile("wfi"); }
        }
        klog_puts("Kernel file verify warn-only mode: continuing.\n");
    } else if (kv_file > 0){
        klog_puts("Kernel file verify not provisioned yet.\n");
    }

    {
        int kernel_trust_ok = (kv == 0 && kv_file == 0);
        if (!kernel_trust_ok){
            klog_puts("Boot security policy: kernel trust not established.\n");
            klog_puts("Boot security policy: local shell + remote login disabled.\n");
            headless_control_note_boot_stage(2u, 1u);
            while (1){
                asm volatile("wfi");
            }
        }
    }

    if (auth_init() != 0){
        klog_puts("AUTH init failed; local shell + remote login disabled.\n");
        klog_puts("Boot security policy: signed AUTH.BIN/AUTH.SIG/AUTH.PQS required.\n");
        headless_control_note_boot_stage(2u, 1u);
        while (1){
            asm volatile("wfi");
        }
    }
//    check_stack();
//    sdhost_read_block(0, sector);
//    uart_puts("First read OK\n");
//    sdhost_read_block(0, sector);
//    uart_puts("Second read OK\n");//    return;
//    if (sdhost_read_block(2048, sector) != 0){
//        uart_puts("READ FAILED!\n");
//        return;
//    }

//    uart_puts("Read success. First 16 bytes: \n");

//    for (int i = 0; i < 16; i++){
//        uart_puthex(sector[i]);
//    } uart_puts("\n");

//    static unsigned char prog[8192];
//
//    extern unsigned long bss_end;
//    extern unsigned long stack_top;
//    extern unsigned long stack_bottom;
//    uart_puts("bss_end = ");
//    uart_puthex((unsigned long)&bss_end);
//    uart_puts("\n");
//    uart_puts("stack_bottom = ");
//    uart_puthex((unsigned long)&stack_bottom);
//    uart_puts("\n");
//    uart_puts("stack_top = ");
//    uart_puthex((unsigned long)&stack_top);
//    uart_puts("\n");
//
//    check_stack();
//
//    if (fat32_init() != 0){
//        uart_puts("FAT init failed!\n");
//        return;
//    }
//    uart_puts("FAT INITIALIZED!\n");
//    int size = fat32_read_file("PROGRAM BIN", prog, sizeof(prog));
//
//    if (size < 0){
//        uart_puts("File read failed.\n");
//        return;
//    }
//
//    uart_puts("PROGRAM.BIN loaded, size = ");
//    uart_puthex(size);
//    uart_puts("\n");

//    uart_puts("--- END SD PIPELINE ---\n");

//    unsigned long entry = load_program_from_sd();
//    if (entry){
//        execute_program(entry);
//    } else{
//        uart_puts("No valid program loaded\n");
//        return;
//    }
//    
    
    uart_puts("Boot shell: loading SHELL.BIN before Pi0 WiFi takeover...\n");
    headless_control_note_boot_stage(3u, 0u);
    loaded_program_t shell_prog = load_boot_shell_program();
    if (!shell_prog.entry){
        klog_puts("SHELL.BIN not found; boot shell disabled by policy.\n");
        headless_control_note_boot_stage(3u, 1u);
        while (1){
            asm volatile("wfi");
        }
    }
    uart_puts("Boot shell: SHELL.BIN loaded; running Pi0 WiFi autojoin hook...\n");
    pi0_headless_wifi_autojoin();
    (void)remote_login_init();
    uart_puts("Boot shell: creating scheduled user shell...\n");
    int shell_pid = start_boot_shell_process(shell_prog);
    if (shell_pid < 0){
        klog_puts("No boot shell process available; halting in idle loop.\n");
        while (1){
            asm volatile("wfi");
        }
    }

    // -----------------------------
    // OPTION 2: TASK DEMO (COMMENTED)
    // -----------------------------
    /*
    task_create(test_task, (void*)'A');
    task_create(test_task, (void*)'B');
    task_run_all();
    */

    // never reach here normally
    while (1){
        headless_control_poll();
        kernel_poll_background_io();
        if (scheduler_has_runnable()){
            scheduler_run_once();
        } else{
            usb_host_service();
            headless_control_poll();
            kernel_poll_background_io();
            asm volatile("wfi");
        }
    }
}
