#include "headless_control.h"
#include "platform/board_config.h"
#include "cpu.h"
#include "gpio.h"
#include "timer.h"
#include "process.h"
#include "cyw43.h"
#include "tcp.h"
#include "blockdev.h"
#include "fat32.h"
#include "sandbox_file.h"
#include "memory.h"
#include "uart.h"
#include "spinlock.h"

#define BTN_DEBOUNCE_MS       35u
#define BTN_DOUBLE_WINDOW_MS  420u
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
#define LED_PULSE_PRE_OFF_MS  120u
#define LED_SCANNER_REFRESH_MS 100u
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
static unsigned long g_btn_raw_tick = 0u;
static unsigned long g_btn_press_tick = 0u;
static unsigned long g_btn_click_deadline = 0u;

static unsigned int g_led_burst_pulses = 0u;
static unsigned int g_led_burst_on = 0u;
static unsigned int g_led_burst_pre_off = 0u;
static unsigned long g_led_burst_next_tick = 0u;
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
static unsigned int g_pending_action = 0u;
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

static int scanner_pick_strongest_open_ssid(char out_ssid[33], int* out_sig){
    static const char aps_path[] = "aps.log";
    int size;
    unsigned char* buf;
    int n;
    int best_live_sig = -200;
    int best_any_sig = -200;
    unsigned long long best_live_last_ms = 0ull;
    char best_live_ssid[33];
    char best_any_ssid[33];
    int found = 0;
    int found_live = 0;

    if (!out_ssid || !out_sig){
        return -1;
    }
    out_ssid[0] = 0;
    *out_sig = -127;
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
                unsigned long long last_ms = 0ull;
                int have_last_ms = 0;
                char ssid[33];
                buf[i] = 0u;
                if (text_find(line, " enc=OPEN") &&
                    parse_quoted_ssid(line, ssid, sizeof(ssid)) == 0 &&
                    parse_i32_key(line, " sig_max=", &sig_max) == 0 &&
                    is_plausible_rssi(sig_max)){
                    if (parse_u64_key(line, " last_ms=", &last_ms) == 0){
                        have_last_ms = 1;
                    }
                    if (have_last_ms && last_ms > 0ull){
                        if (!found_live ||
                            last_ms > best_live_last_ms ||
                            (last_ms == best_live_last_ms && sig_max > best_live_sig)){
                            best_live_last_ms = last_ms;
                            best_live_sig = sig_max;
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
        return 0;
    }
    for (unsigned int i = 0u; i < 33u; i++){
        out_ssid[i] = best_any_ssid[i];
        if (best_any_ssid[i] == 0){
            break;
        }
    }
    *out_sig = best_any_sig;
    return 0;
}

static int headless_https_probe_open_ap(void){
    unsigned char dst_ip[4] = {1u, 1u, 1u, 1u};
    static const char host[] = "one.one.one.one";
    static const char path[] = "/";
    char ssid[33];
    int sig = -127;
    int rc;
    int scanner_running = (find_scanner_pid() >= 0) ? 1 : 0;
    unsigned char resp[1024];

    if (scanner_running){
        /*
         * Ask scanner loop to park briefly (no hop/rearm/recv) before we
         * switch monitor -> station for the probe.
         */
        probe_pause_set(1u);
        headless_control_note_scanner_idle(1u);
        probe_pause_wait_ticks(40u);
    }

    if (scanner_pick_strongest_open_ssid(ssid, &sig) != 0){
        if (scanner_running){
            probe_pause_set(0u);
            headless_control_note_scanner_idle(0u);
        }
        uart_puts("Headless: probe no open AP candidate\n");
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

    (void)cyw43_raw_capture_set_enabled(0u);
    (void)cyw43_ioctl_monitor(0u, 0u);

    rc = cyw43_ioctl_up();
    if (rc != 0){
        uart_puts("Headless: probe wifi up failed rc=");
        uart_putdec((unsigned long)(-rc));
        uart_puts("\n");
        goto probe_restore;
    }

    rc = cyw43_ioctl_join(ssid, "");
    if (rc != 0){
        uart_puts("Headless: probe open join failed rc=");
        uart_putdec((unsigned long)(-rc));
        uart_puts("\n");
        goto probe_restore;
    }

    rc = tcp_https_stream_start(dst_ip, host, path, resp, sizeof(resp));
    if (rc > 0){
        uart_puts("Headless: probe HTTPS OK bytes=");
        uart_putdec((unsigned long)rc);
        uart_puts("\n");
        tcp_https_stream_close();
        rc = 0;
    } else{
        tcp_https_stream_close();
        uart_puts("Headless: probe HTTPS failed rc=");
        uart_putdec((unsigned long)(-rc));
        uart_puts("\n");
    }

probe_restore:
    if (scanner_running){
        headless_control_note_scanner_recovery(1u);
        (void)cyw43_ioctl_up_monitor();
        (void)cyw43_ioctl_monitor(2u, HEADLESS_PROBE_RETURN_CH);
        (void)cyw43_raw_capture_set_enabled(1u);
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

static void led_burst(unsigned int pulses, unsigned long now){
    g_led_burst_pulses = pulses;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 1u;
    g_led_burst_next_tick = now + LED_PULSE_PRE_OFF_MS;
    led_apply(0u);
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
    return 0;
}

static void scanner_stop(void){
    int pid = find_scanner_pid();
    if (pid >= 0){
        process_exit(pid);
    }
}

static void scanner_secure_stop(unsigned long now){
    int wiped = 0;
    int failed = 0;

    scanner_stop();
    (void)cyw43_raw_capture_set_enabled(0u);
    (void)cyw43_ioctl_monitor(0u, 0u);

    if (!blockdev_is_emmc()){
        (void)blockdev_reinit_emmc_from_wifi();
    }
    if (blockdev_is_emmc()){
        fat32_reset();
        if (fat32_init() == 0){
            for (unsigned int i = 0u;
                 i < sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]);
                 i++){
                const char* path = g_scanner_log_paths[i];
                (void)sandbox_file_clear(g_scanner_sandbox_83, path);
                if (fat32_secure_wipe_file_in_dir_path_existing(g_scanner_sandbox_83, path) == 0){
                    wiped++;
                } else{
                    failed++;
                }
            }
        } else{
            failed = (int)(sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]));
        }
    } else{
        failed = (int)(sizeof(g_scanner_log_paths) / sizeof(g_scanner_log_paths[0]));
    }

    led_burst(4u, now);
    uart_puts("Headless: scanner secure-stop, logs wiped=");
    uart_putdec((unsigned long)wiped);
    uart_puts(" failed=");
    uart_putdec((unsigned long)failed);
    uart_puts("\n");
    (void)now;
}

static void button_feedback_burst(unsigned int pulses, unsigned long now){
    if (!QOS_HEADLESS_LED_ENABLED || pulses == 0u){
        return;
    }
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    led_burst(pulses, now);
    spin_unlock(&g_headless_lock);
}

static unsigned int handle_button_actions(unsigned long now){
    if (g_btn_armed && g_btn_stable && !g_btn_long_fired){
        if ((unsigned long)(now - g_btn_press_tick) >= BTN_LONG_PRESS_MS){
            g_btn_long_fired = 1u;
            g_btn_clicks = 0u;
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
        /*
         * Intentional no-op for now. Keep a visible two-pulse ack so the
         * PiSugar S button can be tested without attaching UART/HDMI.
         */
        button_feedback_burst(2u, now);
        uart_puts("Headless: button double press\n");
        return;
    }
    if (action == BTN_ACTION_SECURE_STOP){
        uart_puts("Headless: button long press secure stop\n");
        button_feedback_burst(6u, now);
        scanner_secure_stop(now);
        return;
    }
    if (action == BTN_ACTION_SINGLE_CHECK){
        int rc;
        uart_puts("Headless: button single press, HTTPS probe start\n");
        rc = headless_https_probe_open_ap();
        if (rc == 0){
            button_feedback_burst(3u, now);
            uart_puts("Headless: HTTPS probe success\n");
        } else{
            button_feedback_burst(5u, now);
            uart_puts("Headless: HTTPS probe failed\n");
        }
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
                } else if (old_stable && !g_btn_long_fired){
                    g_btn_clicks++;
                    g_btn_click_deadline = now + BTN_DOUBLE_WINDOW_MS;
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
                g_led_burst_next_tick = now + (g_led_burst_pulses ? LED_PULSE_OFF_MS : LED_PULSE_GAP_MS);
            }
        }
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
    g_btn_raw_tick = now;
    g_btn_press_tick = now;
    g_btn_click_deadline = 0u;
    g_led_burst_pulses = 0u;
    g_led_burst_on = 0u;
    g_led_burst_pre_off = 0u;
    g_led_burst_next_tick = now;
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
    g_pending_action = BTN_ACTION_NONE;
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
    g_led_test_active = 0u;
    g_led_manual_mode = 0u;
    spin_unlock(&g_headless_lock);
}

void headless_control_poll(void){
    unsigned long now;
    unsigned int action = BTN_ACTION_NONE;
    unsigned int run_action = BTN_ACTION_NONE;

    if ((!QOS_HEADLESS_BUTTON_ENABLED && !QOS_HEADLESS_LED_ENABLED) || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (spin_trylock(&g_headless_lock)){
        refresh_scanner_running(now, 0u);
        if (QOS_HEADLESS_BUTTON_ENABLED && cpu_get_id() == 0u){
            poll_button_state(now);
            action = handle_button_actions(now);
            if (action != BTN_ACTION_NONE && g_pending_action == BTN_ACTION_NONE){
                g_pending_action = action;
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
            process_current_pid() < 0){
            run_action = g_pending_action;
            g_pending_action = BTN_ACTION_NONE;
            g_action_busy = 1u;
        }
        spin_unlock(&g_headless_lock);
    }

    /*
     * Run heavy button actions only from an idle kernel context. Button events
     * are often detected while a user process is inside a syscall; doing WiFi
     * handoff/TLS work there can park the process that is meant to observe the
     * pause flag.
     */
    if (run_action != BTN_ACTION_NONE){
        perform_button_action(run_action, now);
        spin_lock(&g_headless_lock);
        g_action_busy = 0u;
        spin_unlock(&g_headless_lock);
    }
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
}

static void probe_pause_wait_ticks(unsigned int wait_ms){
    unsigned long start = system_ticks;
    while ((unsigned long)(system_ticks - start) < (unsigned long)wait_ms){
        asm volatile("wfe" : : : "memory");
    }
}
