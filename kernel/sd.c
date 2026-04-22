#include "sd.h"
#include "uart.h"

// Mailbox base (Pi 3)
#define MBOX_BASE 0x3F00B880

#define MBOX_READ   ((volatile unsigned int*)(MBOX_BASE + 0x00))
#define MBOX_STATUS ((volatile unsigned int*)(MBOX_BASE + 0x18))
#define MBOX_WRITE  ((volatile unsigned int*)(MBOX_BASE + 0x20))

#define MBOX_FULL  0x80000000
#define MBOX_EMPTY 0x40000000

#define MBOX_CHANNEL_PROP 8

// Align buffer to 16 bytes (required)
static volatile unsigned int __attribute__((aligned(16))) mbox[36];

// Mailbox call
static int mailbox_call(unsigned char ch) {
    unsigned int r = ((unsigned int)((unsigned long)&mbox) & ~0xF) | (ch & 0xF);

    // Wait until mailbox is not full
    while (*MBOX_STATUS & MBOX_FULL);

    // Send request
    *MBOX_WRITE = r;

    // Wait for response
    while (1) {
        while (*MBOX_STATUS & MBOX_EMPTY);

        if (*MBOX_READ == r) {
            return mbox[1] == 0x80000000;
        }
    }

    return 0;
}

int sd_init(void) {
    uart_puts("Mailbox SD init (firmware-managed)\n");
    return 0;
}

/*
 * Read a 512-byte sector using mailbox "SD read" tag
 * NOTE: This uses undocumented but commonly used firmware interface
 */
int sd_read_block(unsigned int lba, unsigned char *buffer) {

    // Build mailbox message
    mbox[0] = 9 * 4;           // total size
    mbox[1] = 0;               // request

    // Tag: SD read (0x0003000A is commonly used in bare-metal examples)
    mbox[2] = 0x0003000A;      // tag
    mbox[3] = 16;              // value buffer size
    mbox[4] = 16;              // request size

    mbox[5] = lba;             // sector
    mbox[6] = (unsigned int)((unsigned long)buffer);
    mbox[7] = 512;             // length

    mbox[8] = 0;               // end tag

    if (!mailbox_call(MBOX_CHANNEL_PROP)) {
        uart_puts("Mailbox SD read failed\n");
        return -1;
    }

    return 0;
}
