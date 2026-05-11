#ifndef MAILBOX_H
#define MAILBOX_H

/*
 * Firmware property mailbox buffer.
 *
 * Keep this cache-line isolated: mailbox.c performs cache maintenance over
 * the whole buffer before/after GPU firmware calls.
 */
extern volatile unsigned int mbox[64];

int mailbox_call(unsigned char ch);
void mailbox_lock(void);
void mailbox_unlock(void);
void mailbox_enable_runtime_safety(void);
int mailbox_call_locked(unsigned char ch);

int mailbox_set_emmc_clock(unsigned int hz);
int mailbox_set_power_state(unsigned int device_id, unsigned int state);
int mailbox_set_gpio_state(unsigned int pin, unsigned int state);
int mailbox_power_on_usb(void);
int mailbox_get_arm_memory(unsigned int* base_out, unsigned int* size_out);
int mailbox_get_clock_rate(unsigned int clock_id, unsigned int* hz_out);
int mailbox_set_clock_rate(unsigned int clock_id, unsigned int hz);
int mailbox_set_qpu_enabled(unsigned int enabled);
int mailbox_alloc_vc_memory(unsigned int size,
                            unsigned int alignment,
                            unsigned int flags,
                            unsigned int* handle_out);
int mailbox_lock_vc_memory(unsigned int handle, unsigned int* bus_addr_out);
int mailbox_unlock_vc_memory(unsigned int handle);
int mailbox_release_vc_memory(unsigned int handle);
int mailbox_execute_qpu(unsigned int num_qpus,
                        unsigned int control_bus_addr,
                        unsigned int noflush,
                        unsigned int timeout_ms);
int mailbox_get_temperature(unsigned int sensor_id, unsigned int* milli_c_out);
int mailbox_get_throttled(unsigned int* flags_out);

#endif
