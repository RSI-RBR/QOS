#ifndef HEADLESS_CONTROL_H
#define HEADLESS_CONTROL_H

void headless_control_init(void);
void headless_control_poll(void);
void headless_control_led_tick(void);
void headless_control_note_wifi_joined_waiting_login(void);
void headless_control_note_login_success(void);
void headless_control_note_open_network_packet(void);
void headless_control_note_scanner_recovery(unsigned int active);
int headless_led_test(unsigned int blinks, unsigned int on_ms, unsigned int off_ms);
int headless_led_force(unsigned int on);
unsigned int headless_led_status_word(void);

#endif
