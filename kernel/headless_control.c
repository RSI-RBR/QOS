#include "headless_control.h"
#include "platform/board_config.h"
#include "cpu.h"
#include "gpio.h"
#include "timer.h"
#include "process.h"
#include "cyw43.h"
#include "blockdev.h"
#include "fat32.h"
#include "sandbox_file.h"
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
#define LED_SCANNER_REFRESH_MS 100u
#define LED_BOOT_ON_MS        120u
#define LED_BOOT_OFF_MS       120u
#define LED_BOOT_GAP_MS       1200u
#define LED_BOOT_FAIL_ON_MS   260u
#define LED_BOOT_FAIL_OFF_MS  160u
#define LED_BOOT_FAIL_GAP_MS  1800u

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
static unsigned int g_led_scanner_recovery = 0u;
static unsigned long g_led_scanner_recovery_expire = 0u;
static unsigned int g_led_wifi_joined_waiting_login = 0u;
static unsigned int g_led_login_seen = 0u;
static unsigned long g_led_login_wait_anchor = 0u;
static unsigned int g_led_boot_stage = 0u;
static unsigned int g_led_boot_failed = 0u;
static unsigned long g_led_boot_anchor = 0u;
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
    g_led_burst_next_tick = now;
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
        scanner_secure_stop(now);
        return;
    }
    if (action == BTN_ACTION_SINGLE_CHECK){
        /*
         * Future hook: quick HTTPS/internet probe when scanner is running.
         * For now this is a safe visible ack only.
         */
        button_feedback_burst(1u, now);
        uart_puts("Headless: button single press\n");
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
        if (g_led_wifi_joined_waiting_login && !g_led_login_seen){
            unsigned long elapsed = now - g_led_login_wait_anchor;
            unsigned long phase = elapsed % LED_LOGIN_WAIT_PERIOD_MS;
            led_apply((phase < LED_LOGIN_WAIT_ON_MS) ? 1u : 0u);
            return;
        }
        if (g_led_boot_stage != 0u){
            unsigned int code = g_led_boot_stage;
            unsigned long on_ms = g_led_boot_failed ? LED_BOOT_FAIL_ON_MS : LED_BOOT_ON_MS;
            unsigned long off_ms = g_led_boot_failed ? LED_BOOT_FAIL_OFF_MS : LED_BOOT_OFF_MS;
            unsigned long gap_ms = g_led_boot_failed ? LED_BOOT_FAIL_GAP_MS : LED_BOOT_GAP_MS;
            unsigned long pulse_ms;
            unsigned long active_ms;
            unsigned long cycle;
            unsigned long phase;

            if (code > 12u){
                code = 12u;
            }
            pulse_ms = on_ms + off_ms;
            active_ms = pulse_ms * code;
            cycle = active_ms + gap_ms;
            phase = cycle ? ((now - g_led_boot_anchor) % cycle) : 0u;
            if (phase < active_ms && (phase % pulse_ms) < on_ms){
                led_apply(1u);
            } else{
                led_apply(0u);
            }
            return;
        }
        /*
         * Default idle behavior requested: LED solid on when scanner is not
         * running.
         */
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
    g_led_scanner_recovery = 0u;
    g_led_scanner_recovery_expire = 0u;
    g_led_wifi_joined_waiting_login = 0u;
    g_led_login_seen = 0u;
    g_led_login_wait_anchor = now;
    g_led_boot_stage = 0u;
    g_led_boot_failed = 0u;
    g_led_boot_anchor = now;
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
    unsigned long now;
    if (!QOS_HEADLESS_LED_ENABLED || !g_inited){
        return;
    }
    now = headless_now_ms();
    spin_lock(&g_headless_lock);
    g_led_boot_stage = stage;
    g_led_boot_failed = failed ? 1u : 0u;
    g_led_boot_anchor = now;
    g_led_wifi_joined_waiting_login = 0u;
    g_led_burst_pulses = 0u;
    g_led_test_active = 0u;
    g_led_manual_mode = 0u;
    spin_unlock(&g_headless_lock);
}

void headless_control_poll(void){
    unsigned long now;
    unsigned int action = BTN_ACTION_NONE;

    if ((!QOS_HEADLESS_BUTTON_ENABLED && !QOS_HEADLESS_LED_ENABLED) || !g_inited){
        return;
    }
    now = headless_now_ms();
    if (spin_trylock(&g_headless_lock)){
        refresh_scanner_running(now, 0u);
        if (QOS_HEADLESS_BUTTON_ENABLED && cpu_get_id() == 0u){
            poll_button_state(now);
            action = handle_button_actions(now);
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
