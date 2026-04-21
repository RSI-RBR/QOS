#include "mailbox.h"

#define MMIO_BASE       0x3F000000
#define MBOX_BASE       (MMIO_BASE + 0xB880)

#define MBOX_READ       (*(volatile unsigned int*)(MBOX_BASE + 0x0))
#define MBOX_STATUS     (*(volatile unsigned int*)(MBOX_BASE + 0x18))
#define MBOX_WRITE      (*(volatile unsigned int*)(MBOX_BASE + 0x20))

#define MBOX_EMPTY      0x40000000
#define MBOX_FULL       0x80000000

static volatile unsigned int mbox[36] __attribute__((aligned(16)));

int mailbox_call(unsigned char ch){
    unsigned int r = ((unsigned int)((unsigned long)&mbox) & ~0xF) | (ch & 0xF);

    while (MBOX_STATUS & MBOX_FULL);
    MBOX_WRITE = r;

    while (1){
        while (MBOX_STATUS & MBOX_EMPTY);

        if (MBOX_READ == r)
            return mbox[1] == 0x80000000;
    }
}
