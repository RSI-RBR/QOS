#include "headless_control.h"
#include "platform/board_config.h"
#include "cpu.h"
#include "gpio.h"
#include "timer.h"
#include "process.h"
#include "loader.h"
#include "memory.h"
#include "cyw43.h"
#include "net.h"
#include "tcp.h"
#include "uart.h"
#include "spinlock.h"

#define BTN_DEBOUNCE_MS       35u
#define BTN_DOUBLE_WINDOW_MS  420u
#define BTN_LONG_PRESS_MS     1300u
#define LED_SCAN_PERIOD_MS    1000u
#define LED_SCAN_ON_MS        500u
#define LED_OPEN_ALERT_WINDOW_MS 2000u
#define LED_OPEN_ALERT_PERIOD_MS 1000u
#define LED_OPEN_ALERT_ON1_MS 200u
#define LED_OPEN_ALERT_GAP1_MS 100u
#define LED_OPEN_ALERT_ON2_MS 200u
#define LED_PULSE_ON_MS       85u
#define LED_PULSE_OFF_MS      130u
#define LED_PULSE_GAP_MS      220u
#define HTTPS_PROBE_TIMEOUT_MS 900u

static const char g_scanner_program_83[] = "SCANNER BIN";
static const char g_scanner_sandbox_83[11] = {'S','C','A','N','N','E','R',' ',' ',' ',' '};

static unsigned int g_inited = 0u;
static unsigned int g_led_state = 0u;

static unsigned int g_btn_raw = 0u;
static unsigned int g_btn_stable = 0u;
static unsigned int g_btn_long_fired = 0u;
static unsigned int g_btn_clicks = 0u;
static unsigned long g_btn_raw_tick = 0u;
static unsigned long g_btn_press_tick = 0u;
static unsigned long g_btn_click_deadline = 0u;

static unsigned int g_led_burst_pulses = 0u;
static unsigned int g_led_burst_on = 0u;
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
static unsigned long g_led_open_hit_anchor = 0u;
static unsigned long g_led_open_hit_expire = 0u;
static spinlock_t g_headless_lock;

enum {
    BTN_ACTION_NONE = 0u,
    BTN_ACTION_SINGLE_CHECK = 1u,
    BTN_ACTION_TOGGLE = 2u,
    BTN_ACTION_SECURE_STOP = 3u
};

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
}

static void led_burst(unsigned int pulses, unsigned long now){
    g_led_burst_pulses = pulses;
    g_led_burst_on = 0u;
    g_led_burst_next_tick = now;
}

static void led_test_stop(void){
    g_led_test_active = 0u;
    g_led_test_state_on = 0u;
    g_led_test_remaining_toggles = 0u;
    g_led_test_next_tick = system_ticks;
}

static int led_test_start(unsigned int blinks, unsigned int on_ms, unsigned int off_ms){
    unsigned long now = system_ticks;
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
    return 0;
}

static int scanner_start(void){
    loaded_program_t prog = load_program_from_sd_named(g_scanner_program_83);
    int pid = -1;

    if (!prog.entry){
        return -1;
    }

    pid = process_create_loaded(prog);
    if (pid >= 0){
        return pid;
    }

    if (prog.heap_allocated){
        kfree_secure(prog.memory, prog.size);
    } else{
        loader_free_program_memory(prog.memory, prog.size);
    }
    return -1;
}

static void scanner_stop(void){
    int pid = find_scanner_pid();
    if (pid >= 0){
        process_exit(pid);
    }
}

static void scanner_toggle(unsigned long now){
    int pid = find_scanner_pid();
    if (pid >= 0){
        scanner_stop();
        led_burst(1u, now);
        uart_puts("Headless: scanner stopped\n");
        return;
    }
    if (scanner_start() >= 0){
        led_burst(2u, now);
        uart_puts("Headless: scanner started\n");
    } else{
        led_burst(5u, now);
        uart_puts("Headless: scanner start failed\n");
    }
}

static void scanner_secure_stop(unsigned long now){
    scanner_stop();
    (void)cyw43_raw_capture_set_enabled(0u);
    (void)cyw43_ioctl_monitor(0u, 0u);
    led_burst(4u, now);
    uart_puts("Headless: scanner secure-stop\n");
}

static unsigned int count_open_aps(void){
    cyw43_scan_result_t scans[20];
    unsigned int count = 0u;
    unsigned int open = 0u;

    if (cyw43_ioctl_scan(scans, 20u, &count) != 0){
        return 0u;
    }
    for (unsigned int i = 0; i < count; i++){
        if (scans[i].auth == 0u){
            open++;
        }
    }
    return open;
}

static int quick_https_probe(void){
    static unsigned char out[384];
    static const unsigned char ip[4] = {1u, 1u, 1u, 1u};
    int ping_rc = net_ping_gateway(HTTPS_PROBE_TIMEOUT_MS);
    int https_rc = -1;
    if (ping_rc != 0){
        return 0;
    }
    https_rc = tcp_https_get(ip, "one.one.one.one", "/", out, sizeof(out));
    return (https_rc > 0) ? 1 : 0;
}

static void scanner_single_press_check(unsigned long now){
    unsigned int open_count = count_open_aps();
    if (open_count == 0u){
        led_burst(1u, now);
        uart_puts("Headless: single-press check, no open APs\n");
        return;
    }
    if (quick_https_probe()){
        led_burst(3u, now);
        uart_puts("Headless: single-press check, open AP + HTTPS OK\n");
    } else{
        led_burst(2u, now);
        uart_puts("Headless: single-press check, open AP no internet\n");
    }
}

static unsigned int handle_button_actions(unsigned long now){
    if (g_btn_stable && !g_btn_long_fired){
        if ((unsigned long)(now - g_btn_press_tick) >= BTN_LONG_PRESS_MS){
            g_btn_long_fired = 1u;
            g_btn_clicks = 0u;
            return BTN_ACTION_SECURE_STOP;
        }
    }

    if (g_btn_clicks > 0u && (long)(now - g_btn_click_deadline) >= 0){
        unsigned int clicks = g_btn_clicks;
        g_btn_clicks = 0u;
        if (clicks >= 2u){
            return BTN_ACTION_TOGGLE;
        } else{
            return BTN_ACTION_SINGLE_CHECK;
        }
    }
    return BTN_ACTION_NONE;
}

static void perform_button_action(unsigned int action, unsigned long now){
    if (action == BTN_ACTION_TOGGLE){
        scanner_toggle(now);
        return;
    }
    if (action == BTN_ACTION_SECURE_STOP){
        scanner_secure_stop(now);
        return;
    }
    if (action == BTN_ACTION_SINGLE_CHECK){
        scanner_single_press_check(now);
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
            g_btn_stable = g_btn_raw;
            if (g_btn_stable){
                g_btn_press_tick = now;
                g_btn_long_fired = 0u;
            } else{
                if (!g_btn_long_fired){
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

    unsigned int scanner_running = (find_scanner_pid() >= 0) ? 1u : 0u;

    /*
     * Default idle behavior requested: LED solid on when scanner is not
     * running.
     */
    if (!scanner_running){
        g_led_prev_scanner_running = 0u;
        led_apply(1u);
        return;
    }

    if (g_led_open_hit_valid){
        unsigned long remain = g_led_open_hit_expire - now;
        if ((long)remain > 0){
            unsigned long since_anchor = now - g_led_open_hit_anchor;
            unsigned long phase = since_anchor % LED_OPEN_ALERT_PERIOD_MS;
            if (phase < LED_OPEN_ALERT_ON1_MS ||
                (phase >= (LED_OPEN_ALERT_ON1_MS + LED_OPEN_ALERT_GAP1_MS) &&
                 phase < (LED_OPEN_ALERT_ON1_MS + LED_OPEN_ALERT_GAP1_MS + LED_OPEN_ALERT_ON2_MS))){
                led_apply(1u);
            } else{
                led_apply(0u);
            }
            return;
        }
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
        led_apply(0u);
    }

    g_inited = 1u;
    g_btn_raw = 0u;
    g_btn_stable = 0u;
    g_btn_long_fired = 0u;
    g_btn_clicks = 0u;
    g_btn_raw_tick = system_ticks;
    g_btn_press_tick = system_ticks;
    g_btn_click_deadline = 0u;
    g_led_burst_pulses = 0u;
    g_led_burst_on = 0u;
    g_led_burst_next_tick = system_ticks;
    g_led_base_anchor = system_ticks;
    g_led_prev_scanner_running = 0u;
    g_led_manual_mode = 0u;
    g_led_open_hit_valid = 0u;
    g_led_open_hit_anchor = 0u;
    g_led_open_hit_expire = 0u;
    spinlock_init(&g_headless_lock);
    led_test_stop();
    if (QOS_HEADLESS_BUTTON_ENABLED && QOS_HEADLESS_LED_ENABLED){
        uart_puts("Headless: button/LED control enabled\n");
    } else if (QOS_HEADLESS_BUTTON_ENABLED){
        uart_puts("Headless: button control enabled\n");
    } else if (QOS_HEADLESS_LED_ENABLED){
        uart_puts("Headless: LED control enabled\n");
    }
}

void headless_control_poll(void){
    unsigned long now;
    unsigned int action = BTN_ACTION_NONE;

    if ((!QOS_HEADLESS_BUTTON_ENABLED && !QOS_HEADLESS_LED_ENABLED) || !g_inited){
        return;
    }
    now = system_ticks;
    if (spin_trylock(&g_headless_lock)){
        if (QOS_HEADLESS_BUTTON_ENABLED && cpu_get_id() == 0u){
            poll_button_state(now);
            action = handle_button_actions(now);
        }
        if (QOS_HEADLESS_LED_ENABLED){
            render_led(now);
        }
        spin_unlock(&g_headless_lock);
    }

    /*
     * Run potentially heavy button actions outside the lock so LED cadence
     * remains stable even when an action does network/scan work.
     */
    if (action != BTN_ACTION_NONE){
        perform_button_action(action, now);
    }
}

void headless_control_led_tick(void){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = system_ticks;
    if (!spin_trylock(&g_headless_lock)){
        return;
    }
    render_led(now);
    spin_unlock(&g_headless_lock);
}

void headless_control_note_open_network_packet(void){
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = system_ticks;
    spin_lock(&g_headless_lock);
    if (!g_led_open_hit_valid || (long)(g_led_open_hit_expire - now) <= 0){
        g_led_open_hit_anchor = now;
        g_led_open_hit_expire = now + LED_OPEN_ALERT_WINDOW_MS;
        g_led_open_hit_valid = 1u;
    } else{
        unsigned long ext = now + LED_OPEN_ALERT_WINDOW_MS;
        if ((long)(ext - g_led_open_hit_expire) > 0){
            g_led_open_hit_expire = ext;
        }
    }
    spin_unlock(&g_headless_lock);
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
    v |= (g_led_manual_mode & 0x3u) << 4;
    v |= (QOS_HEADLESS_LED_GPIO & 0xFFu) << 8;
    v |= (QOS_HEADLESS_LED_GPIO_ALT & 0xFFu) << 16;
    return v;
}
