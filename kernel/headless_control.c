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

#define BTN_DEBOUNCE_MS       35u
#define BTN_DOUBLE_WINDOW_MS  420u
#define BTN_LONG_PRESS_MS     1300u
#define LED_IDLE_PERIOD_MS    2500u
#define LED_IDLE_ON_MS        60u
#define LED_SCAN_PERIOD_MS    1000u
#define LED_SCAN_ON_MS        500u
#define LED_PULSE_ON_MS       85u
#define LED_PULSE_OFF_MS      130u
#define LED_PULSE_GAP_MS      220u
#define HTTPS_PROBE_TIMEOUT_MS 900u

static const char g_scanner_name_83[] = "SCANNER BIN";

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
    return str83_eq(p->file_sandbox_83, g_scanner_name_83);
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
    g_led_state = on ? 1u : 0u;
}

static void led_burst(unsigned int pulses, unsigned long now){
    g_led_burst_pulses = pulses;
    g_led_burst_on = 0u;
    g_led_burst_next_tick = now;
}

static int scanner_start(void){
    loaded_program_t prog = load_program_from_sd_named(g_scanner_name_83);
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
        if (scans[i].auth == 0u && scans[i].ssid[0] != 0){
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

static void handle_button_actions(unsigned long now){
    if (g_btn_stable && !g_btn_long_fired){
        if ((unsigned long)(now - g_btn_press_tick) >= BTN_LONG_PRESS_MS){
            g_btn_long_fired = 1u;
            g_btn_clicks = 0u;
            scanner_secure_stop(now);
        }
    }

    if (g_btn_clicks > 0u && (long)(now - g_btn_click_deadline) >= 0){
        unsigned int clicks = g_btn_clicks;
        g_btn_clicks = 0u;
        if (clicks >= 2u){
            scanner_toggle(now);
        } else{
            scanner_single_press_check(now);
        }
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
    unsigned int period = scanner_running ? LED_SCAN_PERIOD_MS : LED_IDLE_PERIOD_MS;
    unsigned int on_ms = scanner_running ? LED_SCAN_ON_MS : LED_IDLE_ON_MS;
    unsigned long elapsed = now - g_led_base_anchor;
    if (elapsed >= period){
        g_led_base_anchor = now;
        elapsed = 0u;
    }
    led_apply((elapsed < on_ms) ? 1u : 0u);
}

void headless_control_init(void){
    if (!QOS_HEADLESS_BUTTON_ENABLED){
        return;
    }
    gpio_set_input(QOS_HEADLESS_BUTTON_GPIO);
    gpio_set_pull(QOS_HEADLESS_BUTTON_GPIO, GPIO_PULL_UP);
    gpio_set_output(QOS_HEADLESS_LED_GPIO);
    led_apply(0u);

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
    uart_puts("Headless: button/LED control enabled\n");
}

void headless_control_poll(void){
    unsigned long now;

    if (!QOS_HEADLESS_BUTTON_ENABLED || !g_inited){
        return;
    }
    if (cpu_get_id() != 0u){
        return;
    }

    now = system_ticks;
    poll_button_state(now);
    handle_button_actions(now);
    render_led(now);
}
