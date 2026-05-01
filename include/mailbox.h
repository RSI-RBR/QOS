#ifndef MAILBOX_H
#define MAILBOX_H

extern volatile unsigned int mbox[36];

int mailbox_call(unsigned char ch);

int mailbox_set_emmc_clock(unsigned int hz);
int mailbox_set_power_state(unsigned int device_id, unsigned int state);
int mailbox_power_on_usb(void);

#endif
