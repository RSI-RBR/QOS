#ifndef MAILBOX_H
#define MAILBOX_H

extern volatile unsigned int mbox[36];

int mailbox_call(unsigned char ch);
void mailbox_lock(void);
void mailbox_unlock(void);
int mailbox_call_locked(unsigned char ch);

int mailbox_set_emmc_clock(unsigned int hz);
int mailbox_set_power_state(unsigned int device_id, unsigned int state);
int mailbox_set_gpio_state(unsigned int pin, unsigned int state);
int mailbox_power_on_usb(void);
int mailbox_get_arm_memory(unsigned int* base_out, unsigned int* size_out);
int mailbox_get_clock_rate(unsigned int clock_id, unsigned int* hz_out);
int mailbox_set_clock_rate(unsigned int clock_id, unsigned int hz);
int mailbox_get_temperature(unsigned int sensor_id, unsigned int* milli_c_out);
int mailbox_get_throttled(unsigned int* flags_out);

#endif
