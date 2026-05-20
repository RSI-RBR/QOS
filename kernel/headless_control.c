#include "headless_control.h"
#include "platform/board_config.h"
#include "cpu.h"
#include "gpio.h"
#include "timer.h"
#include "process.h"
#include "cyw43.h"
#include "tcp.h"
#include "dhcp.h"
#include "arp.h"
#include "net_proto.h"
#include "net.h"
#include "x509_verify.h"
#include "blockdev.h"
#include "fat32.h"
#include "sandbox_file.h"
#include "memory.h"
#include "uart.h"
#include "spinlock.h"
#include "terminal.h"
#include "scanner_log.h"
#include "interrupt.h"

#define BTN_DEBOUNCE_MS       35u
#define BTN_DOUBLE_WINDOW_MS  800u
#define BTN_LONG_PRESS_MS     1300u
#define LED_SCAN_PERIOD_MS    1000u
#define LED_SCAN_ON_MS        500u
#define LED_LOGIN_WAIT_PERIOD_MS 200u
#define LED_LOGIN_WAIT_ON_MS  100u
#define LED_OPEN_ALERT_WINDOW_MS 5000u
#define LED_OPEN_ALERT_ON1_MS 350u
#define LED_OPEN_ALERT_GAP1_MS 180u
#define LED_OPEN_ALERT_ON2_MS 350u
#define LED_OPEN_ALERT_GAP2_MS 1120u
#define LED_PULSE_ON_MS       85u
#define LED_PULSE_OFF_MS      130u
#define LED_PULSE_GAP_MS      220u
#define LED_RESULT_FINAL_GAP_MS 500u
#define LED_PULSE_PRE_OFF_MS  120u
#define LED_ACTION_BUSY_PERIOD_MS 200u
#define LED_ACTION_BUSY_ON_MS  100u
#define LED_SCANNER_REFRESH_MS 100u
#define BTN_PENDING_TIMEOUT_MS 15000u
#define BTN_SAVE_ACK_GRACE_MS  1200u
#define HEADLESS_PROBE_SCANLOG_MAX_BYTES (128u * 1024u)
#define HEADLESS_PROBE_RETURN_CH 6u

static const char g_scanner_sandbox_83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};
static const char* const g_scanner_log_paths[] = {
    "counts.log",
    "aps.log",
    "handshakes.log"
};

static unsigned int g_inited = 0u;
static unsigned int g_led_state = 0u;
static unsigned int g_led_hw_known = 0u;

static unsigned int g_btn_raw = 0u;
static unsigned int g_btn_stable = 0u;
static unsigned int g_btn_armed = 0u;
static unsigned int g_btn_long_fired = 0u;
static unsigned int g_btn_clicks = 0u;
static unsigned int g_btn_last_action = 0u;
static unsigned int g_btn_single_count = 0u;
static unsigned int g_btn_double_count = 0u;
static unsigned int g_btn_long_count = 0u;
static unsigned int g_btn_queued_action = 0u;
static unsigned long g_btn_raw_tick = 0u;
static unsigned long g_btn_press_tick = 0u;
static unsigned long g_btn_click_deadline = 0u;

static unsigned int g_led_burst_pulses = 0u;
static unsigned int g_led_burst_on = 0u;
static unsigned int g_led_burst_pre_off = 0u;
static unsigned long g_led_burst_next_tick = 0u;
static unsigned long g_led_burst_hold_until = 0u;
static unsigned int g_led_burst_final_gap_ms = LED_PULSE_GAP_MS;
static unsigned long g_led_base_anchor = 0u;
static unsigned int g_led_prev_scanner_running = 0u;
static unsigned int g_led_manual_mode = 0u; /* 0=auto, 1=force-off, 2=force-on */
static unsigned int g_led_test_active = 0u;
static unsigned int g_led_test_state_on = 0u;
static unsigned int g_led_test_remaining_toggles = 0u;
static unsigned int g_led_test_on_ms = 500u;
static unsigned int g_led_test_off_ms = 500u;
static unsigned long g_led_test_next_tick = 0u;
static unsigned int g_led_open_hit_valid = 0u;
static unsigned long g_led_open_hit_expire = 0u;
static unsigned int g_led_open_step = 0u;
static unsigned long g_led_open_next_tick = 0u;
static unsigned long g_led_last_render_tick = 0u;
static unsigned int g_led_scanner_running_cached = 0u;
static unsigned long g_led_scanner_next_check = 0u;
static unsigned int g_led_scanner_idle_hint = 0u;
static unsigned int g_led_scanner_recovery = 0u;
static unsigned long g_led_scanner_recovery_expire = 0u;
static unsigned int g_led_wifi_joined_waiting_login = 0u;
static unsigned int g_led_login_seen = 0u;
static unsigned long g_led_login_wait_anchor = 0u;
static unsigned int g_led_boot_stage = 0u;
static unsigned int g_led_boot_failed = 0u;
static unsigned long g_led_boot_anchor = 0u;
static volatile unsigned int g_probe_pause_active = 0u;
static volatile unsigned int g_probe_pause_ack = 0u;
static unsigned int g_pending_action = 0u;
static unsigned long g_pending_action_tick = 0u;
static unsigned int g_action_busy = 0u;
static spinlock_t g_headless_lock;

enum {
    BTN_ACTION_NONE = 0u,
    BTN_ACTION_SINGLE_CHECK = 1u,
    BTN_ACTION_TOGGLE = 2u,
    BTN_ACTION_SECURE_STOP = 3u
};

static int find_scanner_pid(void);
static void probe_pause_set(unsigned int active);
static void probe_pause_wait_ticks(unsigned int wait_ms);
static void headless_control_poll_internal(unsigned int allow_actions);

static void headless_tty0_write(const char* s){
    unsigned long len = 0;
    if (!s){
        return;
    }
    while (s[len]){
        len++;
    }
    terminal_write(0, -1, s, len);
}

static void headless_tty0_show(void){
    (void)terminal_set_active(0);
}

static void headless_tty0_putdec(unsigned long v){
    char tmp[21];
    char out[21];
    int n = 0;
    int o = 0;

    if (v == 0UL){
        headless_tty0_write("0");
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
    headless_tty0_write(out);
}

static void headless_tty0_puti(int v){
    if (v < 0){
        headless_tty0_write("-");
        headless_tty0_putdec((unsigned long)(-v));
    } else{
        headless_tty0_putdec((unsigned long)v);
    }
}

static void headless_tty0_puthex_byte(unsigned char v){
    static const char hex[] = "0123456789ABCDEF";
    char out[3];
    out[0] = hex[(v >> 4) & 0xFu];
    out[1] = hex[v & 0xFu];
    out[2] = 0;
    headless_tty0_write(out);
}

static void headless_tty0_puthex32(unsigned int v){
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4){
        char one[2];
        one[0] = hex[(v >> (unsigned int)shift) & 0xFu];
        one[1] = 0;
        headless_tty0_write(one);
    }
}

static void headless_tty0_write_escaped(const unsigned char* data, unsigned int len){
    if (!data || len == 0u){
        return;
    }
    for (unsigned int i = 0u; i < len; i++){
        unsigned char c = data[i];
        if (c == '\n'){
            headless_tty0_write("\n");
        } else if (c == '\r'){
            headless_tty0_write("\\r");
        } else if (c == '\t'){
            headless_tty0_write("\t");
        } else if (c >= 32u && c <= 126u){
            char one[2];
            one[0] = (char)c;
            one[1] = 0;
            headless_tty0_write(one);
        } else{
            headless_tty0_write("\\x");
            headless_tty0_puthex_byte(c);
        }
    }
}

static int text_eq(const char* a, const char* b){
    if (!a || !b){
        return 0;
    }
    while (*a && *b){
        if (*a != *b){
            return 0;
        }
        a++;
        b++;
    }
    return (*a == 0 && *b == 0) ? 1 : 0;
}

static const char* text_find(const char* s, const char* needle){
    if (!s || !needle || !*needle){
        return 0;
    }
    for (; *s; s++){
        const char* a = s;
        const char* b = needle;
        while (*a && *b && *a == *b){
            a++;
            b++;
        }
        if (!*b){
            return s;
        }
    }
    return 0;
}

static int parse_i32_key(const char* line, const char* key, int* out){
    const char* p;
    const char* k;
    int neg = 0;
    int v = 0;
    int seen = 0;
    if (!line || !key || !out){
        return -1;
    }
    p = text_find(line, key);
    if (!p){
        return -1;
    }
    k = key;
    while (*k){
        k++;
    }
    p += (k - key);
    if (*p == '-'){
        neg = 1;
        p++;
    } else if (*p == '+'){
        p++;
    }
    while (*p >= '0' && *p <= '9'){
        int d = *p - '0';
        v = (v * 10) + d;
        p++;
        seen = 1;
    }
    if (!seen){
        return -1;
    }
    *out = neg ? -v : v;
    return 0;
}

static int parse_u64_key(const char* line, const char* key, unsigned long long* out){
    const char* p;
    const char* k;
    unsigned long long v = 0ull;
    int seen = 0;
    if (!line || !key || !out){
        return -1;
    }
    p = text_find(line, key);
    if (!p){
        return -1;
    }
    k = key;
    while (*k){
        k++;
    }
    p += (k - key);
    while (*p >= '0' && *p <= '9'){
        unsigned int d = (unsigned int)(*p - '0');
        v = (v * 10ull) + (unsigned long long)d;
        p++;
        seen = 1;
    }
    if (!seen){
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_quoted_ssid(const char* line, char* out, unsigned int out_cap){
    const char* p;
    unsigned int n = 0u;
    if (!line || !out || out_cap < 2u){
        return -1;
    }
    p = text_find(line, " ssid=\"");
    if (!p){
        return -1;
    }
    p += 7;
    while (*p && *p != '"' && n + 1u < out_cap){
        char c = *p++;
        if (c == '\\' && *p){
            c = *p++;
        }
        out[n++] = c;
    }
    out[n] = 0;
    if (n == 0u || text_eq(out, "<hidden>")){
        return -1;
    }
    return 0;
}

static int is_plausible_rssi(int sig){
    return (sig >= -110 && sig <= -1) ? 1 : 0;
}

static int scanner_pick_strongest_open_ssid(char out_ssid[33], int* out_sig, unsigned int* out_channel){
    static const char aps_path[] = "aps.log";
    int size;
    unsigned char* buf;
    int n;
    int best_live_sig = -200;
    int best_any_sig = -200;
    unsigned long long best_live_last_ms = 0ull;
    unsigned int best_live_channel = 0u;
    unsigned int best_any_channel = 0u;
    char best_live_ssid[33];
    char best_any_ssid[33];
    int found = 0;
    int found_live = 0;

    if (!out_ssid || !out_sig){
        return -1;
    }
    out_ssid[0] = 0;
    *out_sig = -127;
    if (out_channel){
        *out_channel = 0u;
    }
    best_live_ssid[0] = 0;
    best_any_ssid[0] = 0;

    size = sandbox_file_size(g_scanner_sandbox_83, aps_path);
    if (size <= 0){
        return -1;
    }
    if ((unsigned int)size > HEADLESS_PROBE_SCANLOG_MAX_BYTES){
        size = (int)HEADLESS_PROBE_SCANLOG_MAX_BYTES;
    }

    buf = (unsigned char*)kmalloc((unsigned long)size + 1u);
    if (!buf){
        return -1;
    }
    n = sandbox_file_read(g_scanner_sandbox_83, aps_path, buf, (unsigned int)size);
    if (n <= 0){
        kfree_secure(buf, (unsigned long)size + 1u);
        return -1;
    }
    buf[n] = 0u;

    {
        char* line = (char*)buf;
        for (int i = 0; i <= n; i++){
            if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0u){
                unsigned char saved = buf[i];
                int sig_max;
                int have_sig;
                int ch_i = 0;
                unsigned long long last_ms = 0ull;
                int have_last_ms = 0;
                char ssid[33];
                buf[i] = 0u;
                if (text_find(line, " enc=OPEN") &&
                    parse_quoted_ssid(line, ssid, sizeof(ssid)) == 0){
                    have_sig = (parse_i32_key(line, " sig_max=", &sig_max) == 0 &&
                                is_plausible_rssi(sig_max)) ? 1 : 0;
                    if (!have_sig){
                        sig_max = -127;
                    }
                    if (parse_u64_key(line, " last_ms=", &last_ms) == 0){
                        have_last_ms = 1;
                    }
                    (void)parse_i32_key(line, " ch=", &ch_i);
                    if (have_last_ms && last_ms > 0ull){
                        if (!found_live ||
                            sig_max > best_live_sig ||
                            (sig_max == best_live_sig && last_ms > best_live_last_ms)){
                            best_live_last_ms = last_ms;
                            best_live_sig = sig_max;
                            best_live_channel = (ch_i > 0 && ch_i <= 14) ? (unsigned int)ch_i : 0u;
                            for (unsigned int j = 0u; j < sizeof(best_live_ssid); j++){
                                best_live_ssid[j] = ssid[j];
                                if (ssid[j] == 0){
                                    break;
                                }
                            }
                            found_live = 1;
                        }
                    } else if (!found || sig_max > best_any_sig){
                        best_any_sig = sig_max;
                        best_any_channel = (ch_i > 0 && ch_i <= 14) ? (unsigned int)ch_i : 0u;
                        for (unsigned int j = 0u; j < sizeof(best_any_ssid); j++){
                            best_any_ssid[j] = ssid[j];
                            if (ssid[j] == 0){
                                break;
                            }
                        }
                        found = 1;
                    }
                }
                if (saved == 0u){
                    break;
                }
                line = (char*)&buf[i + 1];
            }
        }
    }

    kfree_secure(buf, (unsigned long)size + 1u);
    if (!found_live && !found){
        return -1;
    }
    if (found_live){
        for (unsigned int i = 0u; i < 33u; i++){
            out_ssid[i] = best_live_ssid[i];
            if (best_live_ssid[i] == 0){
                break;
            }
        }
        *out_sig = best_live_sig;
        if (out_channel){
            *out_channel = best_live_channel;
        }
        return 0;
    }
    for (unsigned int i = 0u; i < 33u; i++){
        out_ssid[i] = best_any_ssid[i];
        if (best_any_ssid[i] == 0){
            break;
        }
    }
    *out_sig = best_any_sig;
    if (out_channel){
        *out_channel = best_any_channel;
    }
    return 0;
}

static void probe_print_net_diag(const char* prefix){
    unsigned long rx_ok = 0;
    unsigned long rx_drop = 0;
    unsigned long tx_ok = 0;
    unsigned long tx_fail = 0;
    unsigned int rxq = 0;
    const char* driver = 0;
    int link_up = 0;

    net_get_diag(&rx_ok, &rx_drop, &tx_ok, &tx_fail, &rxq, &driver, &link_up);
    headless_tty0_write(prefix ? prefix : "Probe: net");
    headless_tty0_write(" drv=");
    headless_tty0_write(driver ? driver : "none");
    headless_tty0_write(" link=");
    headless_tty0_putdec((unsigned long)(link_up ? 1u : 0u));
    headless_tty0_write(" rx=");
    headless_tty0_putdec(rx_ok);
    headless_tty0_write(" drop=");
    headless_tty0_putdec(rx_drop);
    headless_tty0_write(" tx=");
    headless_tty0_putdec(tx_ok);
    headless_tty0_write(" txfail=");
    headless_tty0_putdec(tx_fail);
    headless_tty0_write(" rxq=");
    headless_tty0_putdec((unsigned long)rxq);
    headless_tty0_write("\n");
}

static void headless_tty0_putip(const unsigned char ip[4]){
    if (!ip){
        headless_tty0_write("0.0.0.0");
        return;
    }
    headless_tty0_putdec((unsigned long)ip[0]);
    headless_tty0_write(".");
    headless_tty0_putdec((unsigned long)ip[1]);
    headless_tty0_write(".");
    headless_tty0_putdec((unsigned long)ip[2]);
    headless_tty0_write(".");
    headless_tty0_putdec((unsigned long)ip[3]);
}

static void headless_tty0_putmac(const unsigned char mac[6]){
    if (!mac){
        headless_tty0_write("00:00:00:00:00:00");
        return;
    }
    for (unsigned int i = 0u; i < 6u; i++){
        if (i != 0u){
            headless_tty0_write(":");
        }
        headless_tty0_puthex_byte(mac[i]);
    }
}

static void probe_print_dhcp_diag(void){
    dhcp_diag_t d;
    unsigned long raw_udp68 = 0;
    unsigned long raw_udp67 = 0;
    unsigned long raw_bootp = 0;
    dhcp_get_diag(&d);
    net_proto_get_dhcp_rx_diag(&raw_udp68, &raw_udp67, &raw_bootp);

    headless_tty0_write("Probe: DHCP diag stage=");
    headless_tty0_putdec((unsigned long)d.stage);
    headless_tty0_write(" xid=");
    headless_tty0_puthex32(d.xid);
    headless_tty0_write(" mac=");
    headless_tty0_putmac(d.client_mac);
    headless_tty0_write(" txD=");
    headless_tty0_putdec((unsigned long)d.tx_discover);
    headless_tty0_write(" txR=");
    headless_tty0_putdec((unsigned long)d.tx_request);
    headless_tty0_write(" rx68=");
    headless_tty0_putdec((unsigned long)d.rx_udp68);
    headless_tty0_write(" ok=");
    headless_tty0_putdec((unsigned long)d.parse_ok);
    headless_tty0_write(" type=");
    headless_tty0_putdec((unsigned long)d.last_msg_type);
    headless_tty0_write(" offer=");
    headless_tty0_putip(d.last_offer_ip);
    headless_tty0_write(" server=");
    headless_tty0_putip(d.last_server_id);
    headless_tty0_write("\n");

    headless_tty0_write("Probe: DHCP reject len=");
    headless_tty0_putdec((unsigned long)d.bad_len);
    headless_tty0_write(" hdr=");
    headless_tty0_putdec((unsigned long)d.bad_header);
    headless_tty0_write(" xid=");
    headless_tty0_putdec((unsigned long)d.bad_xid);
    headless_tty0_write(" mac=");
    headless_tty0_putdec((unsigned long)d.bad_mac);
    headless_tty0_write(" magic=");
    headless_tty0_putdec((unsigned long)d.bad_magic);
    headless_tty0_write(" opt=");
    headless_tty0_putdec((unsigned long)d.bad_options);
    headless_tty0_write(" yiaddr=");
    headless_tty0_putdec((unsigned long)d.bad_yiaddr);
    headless_tty0_write(" wrong=");
    headless_tty0_putdec((unsigned long)d.wrong_type);
    headless_tty0_write(" sendfail=");
    headless_tty0_putdec((unsigned long)d.send_fail);
    headless_tty0_write("\n");

    headless_tty0_write("Probe: DHCP raw rx udp68=");
    headless_tty0_putdec(raw_udp68);
    headless_tty0_write(" udp67=");
    headless_tty0_putdec(raw_udp67);
    headless_tty0_write(" bootp=");
    headless_tty0_putdec(raw_bootp);
    headless_tty0_write("\n");
}

static int probe_station_settle(unsigned int ms){
    unsigned long start = system_ticks;
    unsigned int loops = 0u;
    unsigned int max_loops = (ms * 80u) + 4000u;

    while ((unsigned long)(system_ticks - start) < (unsigned long)ms &&
           loops < max_loops){
        (void)net_poll();
        /*
         * Do not use WFE here: this path may run immediately after a syscall
         * transition, and a missed event can make a short settle look frozen.
         */
        for (volatile unsigned int spin = 0u; spin < 250u; spin++){
            asm volatile("yield" : : : "memory");
        }
        loops++;
    }
    if (loops >= max_loops){
        headless_tty0_write("Probe: station settle bounded out; continuing\n");
        return -1;
    }
    return 0;
}

static int probe_preload_x509_trust_store(void){
    int rc;

    headless_tty0_write("Probe: preloading HTTPS CA roots from SD\n");
    /*
     * Pi 3/Zero-class boards share the EMMC controller between SD storage and
     * CYW43 SDIO. Cache CA_ROOTS before station networking starts so HTTPS
     * verification does not try to read FAT while WiFi owns the bus.
     */
    (void)cyw43_raw_capture_set_enabled(0u);
    (void)cyw43_ioctl_monitor(0u, 0u);
    if (blockdev_reinit_emmc_from_wifi() != 0 || !blockdev_is_emmc()){
        headless_tty0_write("Probe: CA preload storage reclaim failed\n");
        return -1;
    }
    fat32_reset();
    x509_verify_reset_cache();
    rc = x509_verify_preload_trust_store();
    if (rc != 0){
        headless_tty0_write("Probe: CA preload failed: ");
        headless_tty0_write(x509_verify_last_error());
        headless_tty0_write("\n");
        return -1;
    }
    headless_tty0_write("Probe: HTTPS CA roots ready\n");
    return 0;
}

static int headless_https_probe_open_ap(void){
    unsigned char dst_ip[4] = {1u, 1u, 1u, 1u};
    static const char host[] = "one.one.one.one";
    static const char path[] = "/";
    static const unsigned char nm_fallback_ip[4] = {10u, 42u, 0u, 88u};
    static const unsigned char nm_fallback_gw[4] = {10u, 42u, 0u, 1u};
    unsigned char old_ip[4];
    unsigned char old_gw[4];
    char ssid[33];
    int sig = -127;
    unsigned int return_ch = HEADLESS_PROBE_RETURN_CH;
    int rc;
    int dhcp_rc;
    int restore_net = 0;
    int scanner_running = (find_scanner_pid() >= 0) ? 1 : 0;
    dhcp_lease_t lease;
    unsigned char resp[1024];

    net_proto_get_local_ip(old_ip);
    net_proto_get_gateway_ip(old_gw);

    if (scanner_running){
        /*
         * Button dispatch only reaches this path after the scanner has
         * acknowledged the pause. Do not fake the acknowledgement here or run
         * monitor/storage work reentrantly from inside the CYW43 driver.
         */
        headless_tty0_write("Probe: scanner monitor path parked\n");
    }

    headless_tty0_write("Probe: selecting strongest open AP from RAM log\n");

    if (scanner_pick_strongest_open_ssid(ssid, &sig, &return_ch) != 0){
        if (scanner_running){
            probe_pause_set(0u);
            headless_control_note_scanner_idle(0u);
        }
        uart_puts("Headless: probe no open AP candidate\n");
        headless_tty0_write("Probe: no open AP candidate found\n");
        return -10;
    }

    uart_puts("Headless: probe target open ssid=\"");
    uart_puts(ssid);
    uart_puts("\" sig=");
    if (sig < 0){
        uart_send('-');
        uart_putdec((unsigned long)(-sig));
    } else{
        uart_putdec((unsigned long)sig);
    }
    uart_puts("\n");
    headless_tty0_write("Probe: target open SSID \"");
    headless_tty0_write(ssid);
    headless_tty0_write("\" signal ");
    headless_tty0_puti(sig);
    headless_tty0_write(" dBm\n");
    if (return_ch == 0u || return_ch > 14u){
        return_ch = HEADLESS_PROBE_RETURN_CH;
    }
    headless_tty0_write("Probe: restore channel ");
    headless_tty0_putdec((unsigned long)return_ch);
    headless_tty0_write("\n");

    if (probe_preload_x509_trust_store() != 0){
        rc = -15;
        goto probe_restore;
    }

    headless_tty0_write("Probe: switching WiFi monitor -> station\n");
    (void)cyw43_raw_capture_set_enabled(0u);
    (void)cyw43_ioctl_monitor(0u, 0u);

    rc = cyw43_ioctl_up();
    if (rc != 0){
        uart_puts("Headless: probe wifi up failed rc=");
        uart_putdec((unsigned long)(-rc));
        uart_puts("\n");
        headless_tty0_write("Probe: WiFi up failed rc=");
        headless_tty0_puti(rc);
        headless_tty0_write("\n");
        rc = -11;
        goto probe_restore;
    }
    headless_tty0_write("Probe: WiFi up OK\n");

    headless_tty0_write("Probe: joining open AP\n");
    rc = cyw43_ioctl_join(ssid, "");
    if (rc != 0){
        uart_puts("Headless: probe open join failed rc=");
        uart_putdec((unsigned long)(-rc));
        uart_puts("\n");
        headless_tty0_write("Probe: join failed rc=");
        headless_tty0_puti(rc);
        headless_tty0_write("\n");
        rc = -12;
        goto probe_restore;
    }
    headless_tty0_write("Probe: join OK\n");
    {
        unsigned char probe_mac[6];
        unsigned char forced_mac[6];
        unsigned int forced_valid = 0u;
        unsigned int forced_pending = 0u;
        unsigned int forced_applied = 0u;
        net_proto_get_local_mac(probe_mac);
        headless_tty0_write("Probe: station MAC ");
        headless_tty0_putmac(probe_mac);
        headless_tty0_write("\n");
        (void)cyw43_get_mac_override_status(forced_mac,
                                            &forced_valid,
                                            &forced_pending,
                                            &forced_applied);
        if (forced_valid){
            headless_tty0_write("Probe: random/forced MAC ");
            headless_tty0_putmac(forced_mac);
            headless_tty0_write(" pending=");
            headless_tty0_putdec((unsigned long)forced_pending);
            headless_tty0_write(" fw_applied=");
            headless_tty0_putdec((unsigned long)forced_applied);
            headless_tty0_write("\n");
        }
    }
    headless_tty0_write("Probe: settling station link\n");
    (void)probe_station_settle(1200u);

    headless_tty0_write("Probe: DHCP request using MAC ");
    {
        unsigned char dhcp_mac[6];
        net_proto_get_local_mac(dhcp_mac);
        headless_tty0_putmac(dhcp_mac);
    }
    headless_tty0_write("\n");
    dhcp_rc = dhcp_acquire(12000u, &lease);
    if (dhcp_rc == 0){
        restore_net = 1;
        headless_tty0_write("Probe: DHCP OK; checking gateway ARP\n");
        if (arp_resolve_gateway(1500u) != 0){
            uart_puts("Headless: probe DHCP OK but gateway ARP unresolved\n");
            headless_tty0_write("Probe: gateway ARP unresolved\n");
        } else{
            headless_tty0_write("Probe: gateway ARP OK\n");
        }
    } else{
        uart_puts("Headless: probe DHCP failed rc=");
        uart_putdec((unsigned long)(-dhcp_rc));
        uart_puts("; trying NM shared static fallback\n");
        headless_tty0_write("Probe: DHCP failed rc=");
        headless_tty0_puti(dhcp_rc);
        headless_tty0_write("; trying static 10.42.0.88/24 via 10.42.0.1\n");
        probe_print_dhcp_diag();
        probe_print_net_diag("Probe: after DHCP fail");
        net_proto_set_local_ip(nm_fallback_ip);
        net_proto_set_gateway_ip(nm_fallback_gw);
        restore_net = 1;
        if (arp_resolve_gateway(1800u) != 0){
            headless_tty0_write("Probe: static fallback gateway ARP unresolved; HTTPS skipped\n");
            probe_print_net_diag("Probe: after static ARP fail");
            rc = -13;
            goto probe_restore;
        }
        headless_tty0_write("Probe: static fallback gateway ARP OK\n");
    }

    headless_tty0_write("Probe: HTTPS GET https://one.one.one.one/\n");
    rc = tcp_https_stream_start(dst_ip, host, path, resp, sizeof(resp));
    if (rc > 0){
        uart_puts("Headless: probe HTTPS OK bytes=");
        uart_putdec((unsigned long)rc);
        uart_puts("\n");
        headless_tty0_write("Probe: HTTPS OK bytes=");
        headless_tty0_putdec((unsigned long)rc);
        headless_tty0_write("\n");
        headless_tty0_write("Probe: HTTPS response begin\n");
        headless_tty0_write_escaped(resp, (unsigned int)rc);
        headless_tty0_write("\nProbe: HTTPS response end\n");
        tcp_https_stream_close();
        rc = 0;
    } else{
        tcp_https_stream_close();
        uart_puts("Headless: probe HTTPS failed rc=");
        uart_putdec((unsigned long)(-rc));
        uart_puts("\n");
        headless_tty0_write("Probe: HTTPS failed rc=");
        headless_tty0_puti(rc);
        headless_tty0_write("\n");
        if (rc == -141){
            headless_tty0_write("Probe: X509 error: ");
            headless_tty0_write(x509_verify_last_error());
            headless_tty0_write("\n");
        }
        rc = -14;
    }

probe_restore:
    if (restore_net){
        net_proto_set_local_ip(old_ip);
        net_proto_set_gateway_ip(old_gw);
    }
    if (scanner_running){
        /*
         * Do not synchronously rearm monitor mode in the headless button
         * handler. CYW43 monitor restore can block after station HTTPS, and if
         * it blocks here then the LED/control loop cannot accept the emergency
         * double-click save. Leave the scanner process to rearm/recover its
         * own monitor path after the pause is released.
         */
        headless_tty0_write("Probe: releasing scanner; monitor rearm deferred to scanner\n");
        (void)cyw43_force_release_emmc_for_storage();
        headless_control_note_scanner_recovery(0u);
        probe_pause_set(0u);
        headless_control_note_scanner_idle(0u);
    }
    return rc;
}

static int str83_eq(const char a[11], const char b[11]){
    if (!a || !b){
        return 0;
    }
    for (unsigned int i = 0; i < 11u; i++){
        if (a[i] != b[i]){
            return 0;
        }
    }
    return 1;
}

static int is_scanner_process(const process_t* p){
    if (!p || !p->user_mode){
        return 0;
    }
    if (p->state == PROC_DEAD || p->state == PROC_REAPING){
        return 0;
    }
    if (!p->file_sandbox_enabled){
        return 0;
    }
    return str83_eq(p->file_sandbox_83, g_scanner_sandbox_83);
}

static int find_scanner_pid(void){
    for (int i = 0; i < MAX_PROCESSES; i++){
        process_t* p = get_process(i);
        if (is_scanner_process(p)){
            return p->pid;
        }
    }
    return -1;
}

static void led_apply(unsigned int on){
    unsigned int hw_on = on ? 1u : 0u;
    if (g_led_hw_known && g_led_state == (on ? 1u : 0u)){
        return;
    }
    if (!QOS_HEADLESS_LED_ACTIVE_HIGH){
        hw_on = hw_on ? 0u : 1u;
    }
    gpio_write(QOS_HEADLESS_LED_GPIO, (int)hw_on);
    if (QOS_HEADLESS_LED_GPIO_ALT < 54u){
        unsigned int hw_on_alt = on ? 1u : 0u;
        if (!QOS_HEADLESS_LED_ALT_ACTIVE_HIGH){
            hw_on_alt = hw_on_alt ? 0u : 1u;
        }
        gpio_write(QOS_HEADLESS_LED_GPIO_ALT, (int)hw_on_alt);
    }
    g_led_state = on ? 1u : 0u;
    g_led_hw_known = 1u;
}

static unsigned long headless_now_ms(void){
    unsigned long cnt = 0;
    unsigned long freq = 0;

    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0u){
        return system_ticks;
    }
    return (cnt * 1000u) / freq;
}

static void refresh_scanner_running(unsigned long now, unsigned int force){
    if (!force && (long)(now - g_led_scanner_next_check) < 0){
        return;
    }
    g_led_scanner_running_cached = (find_scanner_pid() >= 0) ? 1u : 0u;
    g_led_scanner_next_check = now + LED_SCANNER_REFRESH_MS;
}

static void led_burst_with_gap(unsigned int pulses,
                               unsigned long now,
                               unsigned int final_gap_ms){
    g_led_burst_pulses = pulses;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 1u;
    g_led_burst_next_tick = now + LED_PULSE_PRE_OFF_MS;
    g_led_burst_hold_until = 0u;
    g_led_burst_final_gap_ms = final_gap_ms ? final_gap_ms : LED_PULSE_GAP_MS;
    led_apply(0u);
}

static void led_burst(unsigned int pulses, unsigned long now){
    led_burst_with_gap(pulses, now, LED_PULSE_GAP_MS);
}

static void led_test_stop(void){
    g_led_test_active = 0u;
    g_led_test_state_on = 0u;
    g_led_test_remaining_toggles = 0u;
    g_led_test_next_tick = headless_now_ms();
}

static int led_test_start(unsigned int blinks, unsigned int on_ms, unsigned int off_ms){
    unsigned long now = headless_now_ms();
    if (!QOS_HEADLESS_LED_ENABLED){
        return -1;
    }
    if (blinks == 0u){
        blinks = 6u;
    }
    if (blinks > 120u){
        blinks = 120u;
    }
    if (on_ms == 0u){
        on_ms = 500u;
    }
    if (off_ms == 0u){
        off_ms = 500u;
    }
    if (on_ms > 10000u){
        on_ms = 10000u;
    }
    if (off_ms > 10000u){
        off_ms = 10000u;
    }

    g_led_test_on_ms = on_ms;
    g_led_test_off_ms = off_ms;
    g_led_test_active = 1u;
    g_led_manual_mode = 0u;
    g_led_test_state_on = 0u;
    g_led_test_remaining_toggles = blinks * 2u;
    g_led_test_next_tick = now;
    g_led_burst_pulses = 0u;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 0u;
    g_led_burst_hold_until = 0u;
    return 0;
}

static int scanner_stop_and_wait(void){
    int had_scanner = 0;
    int pid = find_scanner_pid();
    if (pid >= 0){
        had_scanner = 1;
        process_exit(pid);
    }

    for (unsigned int i = 0u; i < 200u; i++){
        if (find_scanner_pid() < 0){
            return 0;
        }
        probe_pause_wait_ticks(10u);
    }
    return had_scanner ? -1 : 0;
}

static int scanner_reclaim_storage_for_wipe(void){
    for (unsigned int attempt = 0u; attempt < 4u; attempt++){
        if (blockdev_is_emmc()){
            return 0;
        }
        if (blockdev_reinit_emmc_from_wifi() == 0 && blockdev_is_emmc()){
            return 0;
        }
        probe_pause_wait_ticks(120u);
    }
    return blockdev_is_emmc() ? 0 : -1;
}

static int scanner_secure_stop(unsigned long now){
    int wiped = 0;
    int failed = 0;

    probe_pause_set(1u);
    headless_control_note_scanner_idle(1u);
    if (scanner_stop_and_wait() != 0){
        uart_puts("Headless: scanner secure-stop warning: scanner did not stop cleanly\n");
        failed++;
    }
    (void)cyw43_raw_capture_set_enabled(0u);
    (void)cyw43_ioctl_monitor(0u, 0u);

    (void)scanner_reclaim_storage_for_wipe();
    if (blockdev_is_emmc()){
        fat32_reset();
        if (fat32_init() == 0){
            for (unsigned int i = 0u;
                 i < sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]);
                 i++){
                const char* path = g_scanner_log_paths[i];
                int wipe_rc = fat32_secure_wipe_file_in_dir_path_existing(g_scanner_sandbox_83, path);
                if (wipe_rc == 0 || wipe_rc == -2){
                    wiped++;
                } else{
                    failed++;
                }
                (void)sandbox_file_clear(g_scanner_sandbox_83, path);
            }
        } else{
            failed = (int)(sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]));
        }
    } else{
        failed = (int)(sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]));
    }
    for (unsigned int i = 0u;
         i < sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]);
         i++){
        (void)sandbox_file_clear(g_scanner_sandbox_83, g_scanner_log_paths[i]);
    }

    uart_puts("Headless: scanner secure-stop, logs wiped=");
    uart_putdec((unsigned long)wiped);
    uart_puts(" failed=");
    uart_putdec((unsigned long)failed);
    uart_puts("\n");
    (void)now;
    return (failed == 0) ? 0 : -1;
}

static int scanner_manual_fat_save(unsigned long now){
    int scanner_running = (find_scanner_pid() >= 0) ? 1 : 0;
    int rc;

    (void)now;
    headless_tty0_write("Button: double press scanner FAT save start\n");
    if (scanner_running){
        headless_tty0_write("Save: requesting scanner pause\n");
        probe_pause_set(1u);
        headless_control_note_scanner_idle(1u);
    }

    headless_tty0_write("Save: forcing monitor capture off for storage\n");
    (void)cyw43_force_release_emmc_for_storage();

    headless_tty0_write("Save: flushing encrypted scanner logs to FAT\n");
    rc = scanner_log_flush_all_to_fat_kernel();
    headless_tty0_write("Save: FAT flush rc=");
    headless_tty0_puti(rc);
    headless_tty0_write("\n");

    if (scanner_running){
        headless_tty0_write("Save: releasing scanner; monitor rearm deferred to scanner\n");
        headless_control_note_scanner_recovery(0u);
        probe_pause_set(0u);
        headless_control_note_scanner_idle(0u);
    }

    return (rc == 0) ? 0 : -1;
}

static void button_result_burst(unsigned int pulses, unsigned long now){
    if (!QOS_HEADLESS_LED_ENABLED || pulses == 0u){
        return;
    }
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    led_burst_with_gap(pulses, now, LED_RESULT_FINAL_GAP_MS);
    spin_unlock(&g_headless_lock);
}

static unsigned int handle_button_actions(unsigned long now){
    if (g_btn_queued_action != BTN_ACTION_NONE){
        unsigned int queued = g_btn_queued_action;
        g_btn_queued_action = BTN_ACTION_NONE;
        return queued;
    }

    if (g_btn_armed && g_btn_stable && !g_btn_long_fired){
        if ((unsigned long)(now - g_btn_press_tick) >= BTN_LONG_PRESS_MS){
            g_btn_long_fired = 1u;
            g_btn_clicks = 0u;
            g_btn_click_deadline = 0u;
            g_btn_long_count++;
            g_btn_last_action = BTN_ACTION_SECURE_STOP;
            return BTN_ACTION_SECURE_STOP;
        }
    }

    if (g_btn_clicks > 0u && (long)(now - g_btn_click_deadline) >= 0){
        unsigned int clicks = g_btn_clicks;
        g_btn_clicks = 0u;
        if (clicks == 1u){
            g_btn_single_count++;
            g_btn_last_action = BTN_ACTION_SINGLE_CHECK;
            return BTN_ACTION_SINGLE_CHECK;
        }
        g_btn_double_count++;
        g_btn_last_action = BTN_ACTION_TOGGLE;
        return BTN_ACTION_TOGGLE;
    }
    return BTN_ACTION_NONE;
}

static void perform_button_action(unsigned int action, unsigned long now){
    if (action == BTN_ACTION_TOGGLE){
        int rc;
        headless_tty0_show();
        uart_puts("Headless: button double press\n");
        rc = scanner_manual_fat_save(now);
        button_result_burst((rc == 0) ? 4u : 5u, now);
        return;
    }
    if (action == BTN_ACTION_SECURE_STOP){
        int rc;
        headless_tty0_show();
        uart_puts("Headless: button long press secure stop\n");
        headless_tty0_write("Button: long press secure stop/wipe start\n");
        rc = scanner_secure_stop(now);
        if (rc == 0){
            headless_tty0_write("Button: secure stop/wipe OK\n");
        } else{
            headless_tty0_write("Button: secure stop/wipe failed\n");
        }
        button_result_burst((rc == 0) ? 3u : 5u, now);
        return;
    }
    if (action == BTN_ACTION_SINGLE_CHECK){
        int rc;
        unsigned int pulses = 5u;
        headless_tty0_show();
        uart_puts("Headless: button single press, HTTPS probe start\n");
        headless_tty0_write("Probe: button single press, starting HTTPS open-AP check\n");
        rc = headless_https_probe_open_ap();
        if (rc == 0){
            pulses = 3u;
            uart_puts("Headless: HTTPS probe success\n");
            headless_tty0_write("Probe: success; internet HTTPS reachable\n");
        } else{
            if (rc == -10){
                pulses = 6u; /* no open AP candidate */
            } else if (rc == -11){
                pulses = 7u; /* WiFi up failed */
            } else if (rc == -12){
                pulses = 8u; /* Open AP join failed */
            } else if (rc == -13){
                pulses = 9u; /* DHCP failed + HTTPS failed on fallback */
            } else if (rc == -14){
                pulses = 10u; /* HTTPS failed after join/network setup */
            } else{
                pulses = 5u; /* generic failure */
            }
            uart_puts("Headless: HTTPS probe failed\n");
            headless_tty0_write("Probe: failed stage rc=");
            headless_tty0_puti(rc);
            headless_tty0_write("\n");
        }
        button_result_burst(pulses, now);
        return;
    }
}

static void poll_button_state(unsigned long now){
    int raw_level = gpio_read(QOS_HEADLESS_BUTTON_GPIO);
    unsigned int pressed = (raw_level ? 1u : 0u);

    if (QOS_HEADLESS_BUTTON_ACTIVE_LOW){
        pressed = pressed ? 0u : 1u;
    }

    if (pressed != g_btn_raw){
        g_btn_raw = pressed;
        g_btn_raw_tick = now;
    }

    if (g_btn_stable != g_btn_raw){
        if ((unsigned long)(now - g_btn_raw_tick) >= BTN_DEBOUNCE_MS){
            unsigned int old_stable = g_btn_stable;
            g_btn_stable = g_btn_raw;
            if (g_btn_stable){
                /*
                 * PiSugar S uses GPIO3/SCL. If its auto-start switch is ON or
                 * external power is present, SCL can be low at boot. Do not
                 * arm button actions until we have observed a released state.
                 */
                if (g_btn_armed){
                    g_btn_press_tick = now;
                    g_btn_long_fired = 0u;
                }
            } else{
                if (!g_btn_armed){
                    g_btn_armed = 1u;
                    g_btn_clicks = 0u;
                    g_btn_long_fired = 0u;
                } else if (old_stable){
                    unsigned long held_ms = now - g_btn_press_tick;
                    if (g_btn_long_fired){
                        g_btn_clicks = 0u;
                        g_btn_click_deadline = 0u;
                    } else if (held_ms >= BTN_LONG_PRESS_MS){
                        g_btn_clicks = 0u;
                        g_btn_click_deadline = 0u;
                        g_btn_long_fired = 1u;
                        g_btn_long_count++;
                        g_btn_last_action = BTN_ACTION_SECURE_STOP;
                        g_btn_queued_action = BTN_ACTION_SECURE_STOP;
                    } else{
                        if (g_btn_clicks == 0u){
                            g_btn_clicks = 1u;
                            g_btn_click_deadline = now + BTN_DOUBLE_WINDOW_MS;
                        } else{
                            g_btn_clicks = 0u;
                            g_btn_click_deadline = 0u;
                            g_btn_double_count++;
                            g_btn_last_action = BTN_ACTION_TOGGLE;
                            g_btn_queued_action = BTN_ACTION_TOGGLE;
                        }
                    }
                }
                g_btn_long_fired = 0u;
            }
        }
    }
}

static void render_led(unsigned long now){
    if (g_led_manual_mode == 1u){
        led_apply(0u);
        return;
    }
    if (g_led_manual_mode == 2u){
        led_apply(1u);
        return;
    }

    if (g_led_test_active){
        if ((long)(now - g_led_test_next_tick) >= 0){
            if (!g_led_test_state_on){
                led_apply(1u);
                g_led_test_state_on = 1u;
                g_led_test_next_tick = now + g_led_test_on_ms;
            } else{
                led_apply(0u);
                g_led_test_state_on = 0u;
                if (g_led_test_remaining_toggles > 0u){
                    g_led_test_remaining_toggles--;
                }
                if (g_led_test_remaining_toggles == 0u){
                    led_test_stop();
                } else{
                    g_led_test_next_tick = now + g_led_test_off_ms;
                }
            }
        }
        return;
    }

    if (g_led_burst_pulses > 0u){
        if ((long)(now - g_led_burst_next_tick) >= 0){
            if (g_led_burst_pre_off){
                g_led_burst_pre_off = 0u;
                g_led_burst_on = 0u;
            }
            if (!g_led_burst_on){
                led_apply(1u);
                g_led_burst_on = 1u;
                g_led_burst_next_tick = now + LED_PULSE_ON_MS;
            } else{
                led_apply(0u);
                g_led_burst_on = 0u;
                g_led_burst_pulses--;
                if (g_led_burst_pulses){
                    g_led_burst_next_tick = now + LED_PULSE_OFF_MS;
                } else{
                    g_led_burst_hold_until = now + g_led_burst_final_gap_ms;
                }
            }
        }
        return;
    }
    if ((long)(g_led_burst_hold_until - now) > 0){
        led_apply(0u);
        return;
    }
    g_led_burst_hold_until = 0u;

    if (g_action_busy){
        unsigned long phase = now % LED_ACTION_BUSY_PERIOD_MS;
        led_apply((phase < LED_ACTION_BUSY_ON_MS) ? 1u : 0u);
        return;
    }

    unsigned int scanner_running = g_led_scanner_running_cached;

    if (!scanner_running){
        g_led_prev_scanner_running = 0u;
        g_led_scanner_idle_hint = 0u;
        if (g_led_wifi_joined_waiting_login && !g_led_login_seen){
            unsigned long elapsed = now - g_led_login_wait_anchor;
            unsigned long phase = elapsed % LED_LOGIN_WAIT_PERIOD_MS;
            led_apply((phase < LED_LOGIN_WAIT_ON_MS) ? 1u : 0u);
            return;
        }
        /*
         * Default idle behavior requested: LED solid on when scanner is not
         * running.
         */
        led_apply(1u);
        return;
    }

    if (g_led_scanner_idle_hint){
        g_led_prev_scanner_running = 0u;
        led_apply(1u);
        return;
    }

    if (g_led_scanner_recovery){
        if ((long)(g_led_scanner_recovery_expire - now) > 0){
            g_led_prev_scanner_running = 0u;
            led_apply(1u);
            return;
        }
        g_led_scanner_recovery = 0u;
    }

    if (g_led_open_hit_valid){
        if ((long)(g_led_open_hit_expire - now) > 0){
            unsigned long elapsed = now - g_led_open_next_tick;
            unsigned long cycle = LED_OPEN_ALERT_ON1_MS +
                                  LED_OPEN_ALERT_GAP1_MS +
                                  LED_OPEN_ALERT_ON2_MS +
                                  LED_OPEN_ALERT_GAP2_MS;
            unsigned long phase = cycle ? (elapsed % cycle) : 0u;
            unsigned int on =
                (phase < LED_OPEN_ALERT_ON1_MS) ||
                (phase >= (LED_OPEN_ALERT_ON1_MS + LED_OPEN_ALERT_GAP1_MS) &&
                 phase < (LED_OPEN_ALERT_ON1_MS + LED_OPEN_ALERT_GAP1_MS + LED_OPEN_ALERT_ON2_MS));
            led_apply(on ? 1u : 0u);
            return;
        }
        g_led_open_hit_valid = 0u;
    }

    /*
     * Scanner mode: stable 500/500 phase blink.
     * Use a fixed anchor + modulo timing so brief scheduler stalls don't
     * permanently stretch the blink cadence.
     */
    if (!g_led_prev_scanner_running){
        g_led_base_anchor = now;
        g_led_prev_scanner_running = 1u;
    }
    {
        unsigned long elapsed = now - g_led_base_anchor;
        unsigned long phase = elapsed % LED_SCAN_PERIOD_MS;
        led_apply((phase < LED_SCAN_ON_MS) ? 1u : 0u);
    }
}

void headless_control_init(void){
    unsigned long now;

    if (g_inited){
        return;
    }
    if (!QOS_HEADLESS_BUTTON_ENABLED && !QOS_HEADLESS_LED_ENABLED){
        return;
    }
    if (QOS_HEADLESS_BUTTON_ENABLED){
        gpio_set_input(QOS_HEADLESS_BUTTON_GPIO);
        gpio_set_pull(QOS_HEADLESS_BUTTON_GPIO, GPIO_PULL_UP);
    }
    if (QOS_HEADLESS_LED_ENABLED){
        gpio_set_output(QOS_HEADLESS_LED_GPIO);
        if (QOS_HEADLESS_LED_GPIO_ALT < 54u){
            gpio_set_output(QOS_HEADLESS_LED_GPIO_ALT);
        }
        g_led_hw_known = 0u;
        led_apply(0u);
    }

    g_inited = 1u;
    now = headless_now_ms();
    g_btn_raw = 0u;
    g_btn_stable = 0u;
    g_btn_armed = 0u;
    g_btn_long_fired = 0u;
    g_btn_clicks = 0u;
    g_btn_last_action = BTN_ACTION_NONE;
    g_btn_single_count = 0u;
    g_btn_double_count = 0u;
    g_btn_long_count = 0u;
    g_btn_queued_action = BTN_ACTION_NONE;
    g_btn_raw_tick = now;
    g_btn_press_tick = now;
    g_btn_click_deadline = 0u;
    g_led_burst_pulses = 0u;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 0u;
    g_led_burst_next_tick = now;
    g_led_burst_hold_until = 0u;
    g_led_burst_final_gap_ms = LED_PULSE_GAP_MS;
    g_led_base_anchor = now;
    g_led_prev_scanner_running = 0u;
    g_led_manual_mode = 0u;
    g_led_open_hit_valid = 0u;
    g_led_open_hit_expire = 0u;
    g_led_open_step = 0u;
    g_led_open_next_tick = now;
    g_led_last_render_tick = now;
    g_led_scanner_running_cached = 0u;
    g_led_scanner_next_check = now;
    g_led_scanner_idle_hint = 0u;
    g_led_scanner_recovery = 0u;
    g_led_scanner_recovery_expire = 0u;
    g_led_wifi_joined_waiting_login = 0u;
    g_led_login_seen = 0u;
    g_led_login_wait_anchor = now;
    g_led_boot_stage = 0u;
    g_led_boot_failed = 0u;
    g_led_boot_anchor = now;
    g_probe_pause_active = 0u;
    g_probe_pause_ack = 0u;
    g_pending_action = BTN_ACTION_NONE;
    g_pending_action_tick = now;
    g_action_busy = 0u;
    spinlock_init(&g_headless_lock);
    led_test_stop();
    if (QOS_HEADLESS_LED_ENABLED){
        led_apply(1u);
    }
    if (QOS_HEADLESS_BUTTON_ENABLED && QOS_HEADLESS_LED_ENABLED){
        uart_puts("Headless: button/LED control enabled\n");
    } else if (QOS_HEADLESS_BUTTON_ENABLED){
        uart_puts("Headless: button control enabled\n");
    } else if (QOS_HEADLESS_LED_ENABLED){
        uart_puts("Headless: LED control enabled\n");
    }
}

void headless_control_note_boot_stage(unsigned int stage, unsigned int failed){
    (void)stage;
    (void)failed;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    /*
     * Keep boot visually quiet for headless field use. The LED should be
     * solid by default and only fast-blink when remote login is ready.
     */
    spin_lock(&g_headless_lock);
    g_led_boot_stage = 0u;
    g_led_boot_failed = 0u;
    g_led_wifi_joined_waiting_login = 0u;
    g_led_burst_pulses = 0u;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 0u;
    g_led_burst_hold_until = 0u;
    g_led_test_active = 0u;
    g_led_manual_mode = 0u;
    spin_unlock(&g_headless_lock);
}

static void headless_control_poll_internal(unsigned int allow_actions){
    unsigned long now;
    unsigned int action = BTN_ACTION_NONE;
    unsigned int run_action = BTN_ACTION_NONE;
    unsigned int timeout_notice = 0u;
    unsigned int timeout_reason = 0u;
    unsigned int timeout_action = BTN_ACTION_NONE;
    unsigned int fallback_notice = 0u;
    unsigned int queued_notice = BTN_ACTION_NONE;

    if ((!QOS_HEADLESS_BUTTON_ENABLED && !QOS_HEADLESS_LED_ENABLED) || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (spin_trylock(&g_headless_lock)){
        refresh_scanner_running(now, 0u);
        if (QOS_HEADLESS_BUTTON_ENABLED && cpu_get_id() == 0u){
            poll_button_state(now);
            action = handle_button_actions(now);
            if (action != BTN_ACTION_NONE){
                if (!g_action_busy &&
                    ((action == BTN_ACTION_TOGGLE && g_pending_action == BTN_ACTION_SINGLE_CHECK) ||
                     (action == BTN_ACTION_SECURE_STOP && g_pending_action != BTN_ACTION_NONE))){
                    /*
                     * If the second click arrives after a single-click probe
                     * was queued but before it dispatched, upgrade that pending
                     * action instead of letting the probe run. Long press has
                     * the same priority over any queued scanner action.
                     */
                    unsigned int ack_pulses = (action == BTN_ACTION_SECURE_STOP) ? 6u : 2u;
                    g_pending_action = action;
                    g_pending_action_tick = now;
                    probe_pause_set(1u);
                    g_led_scanner_running_cached = 1u;
                    g_led_scanner_idle_hint = 1u;
                    g_led_open_hit_valid = 0u;
                    g_led_base_anchor = now;
                    g_led_prev_scanner_running = 0u;
                    queued_notice = action;
                    led_burst(ack_pulses, now);
                } else if (g_pending_action == BTN_ACTION_NONE && !g_action_busy){
                    unsigned int ack_pulses = 1u;
                    g_pending_action = action;
                    g_pending_action_tick = now;
                    /*
                     * Scanner-facing actions need the scanner to
                     * park before WiFi/FAT work. Start that pause as soon as
                     * the button is recognized so the deferred action can get
                     * an idle slot instead of waiting behind the scanner.
                     */
                    probe_pause_set(1u);
                    g_led_scanner_running_cached = 1u;
                    g_led_scanner_idle_hint = 1u;
                    g_led_open_hit_valid = 0u;
                    g_led_base_anchor = now;
                    g_led_prev_scanner_running = 0u;
                    if (action == BTN_ACTION_SECURE_STOP){
                        ack_pulses = 6u;
                    } else if (action == BTN_ACTION_TOGGLE){
                        ack_pulses = 2u;
                    }
                    queued_notice = action;
                    led_burst(ack_pulses, now);
                } else{
                    /*
                     * Acknowledge the press even if a previous action is still
                     * pending/busy. This keeps the headless button testable.
                     */
                    led_burst(1u, now);
                }
            }
        }
        if (QOS_HEADLESS_LED_ENABLED){
            /*
             * Fallback refresh path: if timer-driven LED rendering is briefly
             * starved by lock contention, refresh here at most every ~2ms.
             */
            if ((unsigned long)(now - g_led_last_render_tick) >= 2u){
                render_led(now);
                g_led_last_render_tick = now;
            }
        }
        if (g_pending_action != BTN_ACTION_NONE &&
            !g_action_busy &&
            (unsigned long)(now - g_pending_action_tick) >= BTN_PENDING_TIMEOUT_MS){
            timeout_reason = (find_scanner_pid() >= 0 && !g_probe_pause_ack) ? 1u : 2u;
            timeout_action = g_pending_action;
            timeout_notice = 1u;
            g_pending_action = BTN_ACTION_NONE;
            probe_pause_set(0u);
            g_probe_pause_ack = 0u;
            g_led_scanner_idle_hint = 0u;
            led_burst(11u, now);
        }
        if (allow_actions &&
            run_action == BTN_ACTION_NONE &&
            g_pending_action != BTN_ACTION_NONE &&
            !g_action_busy &&
            (g_pending_action == BTN_ACTION_SECURE_STOP ||
             find_scanner_pid() < 0 ||
             g_probe_pause_ack ||
             (g_pending_action == BTN_ACTION_TOGGLE &&
              (unsigned long)(now - g_pending_action_tick) >= BTN_SAVE_ACK_GRACE_MS))){
            if (g_pending_action == BTN_ACTION_TOGGLE && !g_probe_pause_ack && find_scanner_pid() >= 0){
                fallback_notice = 1u;
            }
            run_action = g_pending_action;
            g_pending_action = BTN_ACTION_NONE;
            g_action_busy = 1u;
        }
        spin_unlock(&g_headless_lock);
    }

    if (timeout_notice){
        headless_tty0_show();
        if (timeout_action == BTN_ACTION_SINGLE_CHECK){
            headless_tty0_write("Button: single-click probe ");
        } else if (timeout_action == BTN_ACTION_TOGGLE){
            headless_tty0_write("Button: double-click save ");
        } else if (timeout_action == BTN_ACTION_SECURE_STOP){
            headless_tty0_write("Button: long-press secure stop ");
        } else{
            headless_tty0_write("Button: action ");
        }
        if (timeout_reason == 1u){
            headless_tty0_write("timed out waiting for scanner pause ack\n");
        } else{
            headless_tty0_write("timed out before dispatch\n");
        }
    }
    if (queued_notice != BTN_ACTION_NONE){
        headless_tty0_show();
        if (queued_notice == BTN_ACTION_SINGLE_CHECK){
            headless_tty0_write("Button: queued single-click probe\n");
        } else if (queued_notice == BTN_ACTION_TOGGLE){
            headless_tty0_write("Button: queued double-click save\n");
        } else if (queued_notice == BTN_ACTION_SECURE_STOP){
            headless_tty0_write("Button: queued long-press secure stop\n");
        }
    }
    if (fallback_notice){
        headless_tty0_show();
        headless_tty0_write("Button: scanner pause ack missed; forcing double-click save\n");
    }

    /*
     * Run heavy button actions after the scanner has acknowledged the pause.
     * Waiting for a fully idle CPU made headless button actions time out under
     * continuous scanner/shell load even though the scanner was already parked.
     */
    if (allow_actions && run_action != BTN_ACTION_NONE){
        headless_tty0_show();
        if (run_action == BTN_ACTION_SINGLE_CHECK){
            headless_tty0_write("Button: dispatch single-click probe\n");
        } else if (run_action == BTN_ACTION_TOGGLE){
            headless_tty0_write("Button: dispatch double-click save\n");
        } else if (run_action == BTN_ACTION_SECURE_STOP){
            headless_tty0_write("Button: dispatch long-press secure stop\n");
        }
        kernel_preempt_enter();
        perform_button_action(run_action, now);
        kernel_preempt_exit();
        spin_lock(&g_headless_lock);
        g_action_busy = 0u;
        if (run_action == BTN_ACTION_SINGLE_CHECK ||
            run_action == BTN_ACTION_TOGGLE ||
            run_action == BTN_ACTION_SECURE_STOP){
            probe_pause_set(0u);
            g_probe_pause_ack = 0u;
            g_led_scanner_idle_hint = 0u;
        }
        spin_unlock(&g_headless_lock);
    }
}

void headless_control_poll(void){
    headless_control_poll_internal(0u);
}

void headless_control_poll_actions(void){
    headless_control_poll_internal(1u);
}

void headless_control_led_tick(void){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    render_led(now);
    g_led_last_render_tick = now;
    spin_unlock(&g_headless_lock);
}

void headless_control_note_wifi_joined_waiting_login(void){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    if (!g_led_login_seen){
        g_led_wifi_joined_waiting_login = 1u;
        g_led_login_wait_anchor = now;
        g_led_boot_stage = 0u;
        g_led_boot_failed = 0u;
        g_led_burst_pulses = 0u;
        g_led_test_active = 0u;
        g_led_manual_mode = 0u;
        g_led_burst_hold_until = 0u;
    }
    spin_unlock(&g_headless_lock);
}

void headless_control_note_login_success(void){
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    g_led_login_seen = 1u;
    g_led_wifi_joined_waiting_login = 0u;
    g_led_boot_stage = 0u;
    g_led_boot_failed = 0u;
    spin_unlock(&g_headless_lock);
}

static void headless_note_remote_burst(unsigned int pulses){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    g_led_boot_stage = 0u;
    g_led_boot_failed = 0u;
    g_led_test_active = 0u;
    g_led_manual_mode = 0u;
    led_burst(pulses, now);
    spin_unlock(&g_headless_lock);
}

void headless_control_note_remote_login_rx(void){
    headless_note_remote_burst(1u);
}

void headless_control_note_remote_login_tx(void){
    headless_note_remote_burst(2u);
}

void headless_control_note_remote_login_tx_fail(void){
    headless_note_remote_burst(5u);
}

void headless_control_note_open_network_packet(void){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (!spin_trylock(&g_headless_lock)){
        /*
         * Don't block scanner/network paths on LED bookkeeping. A missed hit
         * here is acceptable because subsequent packets will re-notify.
         */
        return;
    }
    g_led_scanner_running_cached = 1u;
    if (!g_led_open_hit_valid || (long)(g_led_open_hit_expire - now) <= 0){
        g_led_open_hit_expire = now + LED_OPEN_ALERT_WINDOW_MS;
        g_led_open_hit_valid = 1u;
        /*
         * Start the obvious double-blink pattern immediately:
         * long-on, short-off, long-on, long-off. This is intentionally more
         * distinct than the normal scanner 500/500 blink.
         */
        g_led_open_step = 0u;
        g_led_open_next_tick = now;
        led_apply(1u);
    } else{
        unsigned long ext = now + LED_OPEN_ALERT_WINDOW_MS;
        if ((long)(ext - g_led_open_hit_expire) > 0){
            g_led_open_hit_expire = ext;
        }
    }
    spin_unlock(&g_headless_lock);
}

void headless_control_note_scanner_idle(unsigned int active){
    unsigned long now;
    if (active && g_probe_pause_active){
        g_probe_pause_ack = 1u;
    } else if (!active){
        g_probe_pause_ack = 0u;
    }
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    if (active){
        g_led_scanner_running_cached = 1u;
        g_led_scanner_idle_hint = 1u;
        g_led_open_hit_valid = 0u;
        g_led_base_anchor = now;
        g_led_prev_scanner_running = 0u;
        led_apply(1u);
    } else{
        g_led_scanner_idle_hint = 0u;
        g_led_base_anchor = now;
        g_led_prev_scanner_running = 0u;
    }
    spin_unlock(&g_headless_lock);
}

void headless_control_note_scanner_recovery(unsigned int active){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    if (active){
        g_led_scanner_running_cached = 1u;
        g_led_scanner_idle_hint = 1u;
        g_led_scanner_recovery = 1u;
        g_led_scanner_recovery_expire = now + 60000u;
        g_led_open_hit_valid = 0u;
        led_apply(1u);
    } else{
        g_led_scanner_recovery = 0u;
        g_led_scanner_recovery_expire = 0u;
        g_led_base_anchor = now;
        g_led_prev_scanner_running = 0u;
    }
    spin_unlock(&g_headless_lock);
}

int headless_control_probe_pause_active(void){
    return g_probe_pause_active ? 1 : 0;
}

int headless_led_test(unsigned int blinks, unsigned int on_ms, unsigned int off_ms){
    int rc = -1;
    if (cpu_get_id() != 0u){
        return -1;
    }
    spin_lock(&g_headless_lock);
    rc = led_test_start(blinks, on_ms, off_ms);
    spin_unlock(&g_headless_lock);
    return rc;
}

int headless_led_force(unsigned int on){
    if (!QOS_HEADLESS_LED_ENABLED || cpu_get_id() != 0u){
        return -1;
    }
    spin_lock(&g_headless_lock);
    led_test_stop();
    g_led_burst_pulses = 0u;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 0u;
    g_led_burst_hold_until = 0u;
    if (on == 2u){
        g_led_manual_mode = 0u;
        spin_unlock(&g_headless_lock);
        return 0;
    }
    g_led_manual_mode = on ? 2u : 1u;
    led_apply(on ? 1u : 0u);
    spin_unlock(&g_headless_lock);
    return 0;
}

unsigned int headless_led_status_word(void){
    unsigned int v = 0u;
    if (QOS_HEADLESS_LED_ENABLED){
        v |= 1u << 0;
    }
    if (g_led_state){
        v |= 1u << 1;
    }
    if (g_led_test_active){
        v |= 1u << 2;
    }
    if (QOS_HEADLESS_LED_ACTIVE_HIGH){
        v |= 1u << 3;
    }
    if (g_led_wifi_joined_waiting_login){
        v |= 1u << 6;
    }
    if (g_led_login_seen){
        v |= 1u << 7;
    }
    v |= (g_led_manual_mode & 0x3u) << 4;
    v |= (QOS_HEADLESS_LED_GPIO & 0xFFu) << 8;
    v |= (QOS_HEADLESS_LED_GPIO_ALT & 0xFFu) << 16;
    if (QOS_HEADLESS_BUTTON_ENABLED){
        v |= 1u << 24;
    }
    if (g_btn_raw){
        v |= 1u << 25;
    }
    if (g_btn_stable){
        v |= 1u << 26;
    }
    if (g_btn_armed){
        v |= 1u << 27;
    }
    v |= (g_btn_last_action & 0xFu) << 28;
    return v;
}
static void probe_pause_set(unsigned int active){
    g_probe_pause_active = active ? 1u : 0u;
    if (!active){
        g_probe_pause_ack = 0u;
    }
}

static void probe_pause_wait_ticks(unsigned int wait_ms){
    unsigned long start = system_ticks;
    while ((unsigned long)(system_ticks - start) < (unsigned long)wait_ms){
        asm volatile("wfe" : : : "memory");
    }
}
