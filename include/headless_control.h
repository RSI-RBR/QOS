#ifndef HEADLESS_CONTROL_H
#define HEADLESS_CONTROL_H

void headless_control_init(void);
void headless_control_poll(void);
int headless_led_test(unsigned int blinks, unsigned int on_ms, unsigned int off_ms);
int headless_led_force(unsigned int on);
unsigned int headless_led_status_word(void);

#endif
