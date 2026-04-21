#ifndef MAILBOX_H
#define MAILBOX_H

extern volatile unsigned int mbox[36];

int mailbox_call(unsigned char ch);

#endif
