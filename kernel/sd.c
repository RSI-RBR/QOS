#include "sd.h"
#include "uart.h"

#define SDHOST_BASE 0x3F202000

#define SD_CMD   ((volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SD_ARG   ((volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SD_TOUT  ((volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SD_CDIV  ((volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SD_RSP0  ((volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SD_HSTS  ((volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SD_DATA  ((volatile unsigned int*)(SDHOST_BASE + 0x40))

#define SD_CMD_NEW_FLAG (1 << 15)
#define SD_CMD_FAIL_FLAG (1 << 14)
#define SD_CMD_RESPONSE (1 << 6)
#define SD_CMD_READ     (1 << 3)

#define SD_HSTS_BUSY       (1 << 0)
#define SD_HSTS_FIFO_EMPTY (1 << 1)

static int sd_wait_cmd_done(void) {
    int timeout = 1000000;

    while (timeout--) {
        if (!(*SD_CMD & SD_CMD_NEW_FLAG)) {
            if (*SD_CMD & SD_CMD_FAIL_FLAG) {
                uart_puts("CMD failed\n");
                return -1;
            }
            return 0;
        }
    }

    uart_puts("CMD timeout\n");
    return -1;
}

static int sd_send_cmd(unsigned int cmd, unsigned int arg) {

    while (*SD_HSTS & SD_HSTS_BUSY);

    *SD_ARG = arg;
    *SD_CMD = cmd | SD_CMD_NEW_FLAG;

    return sd_wait_cmd_done();
}

int sd_init(void) {
    uart_puts("SD init (firmware pre-initialized)\n");

    // DO NOT reset controller
    // DO NOT send CMD0
    // Firmware already initialized the card

    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buffer) {

    // On Pi firmware setup, card is already SDHC
    unsigned int addr = lba;

    if (sd_send_cmd(17 | SD_CMD_RESPONSE | SD_CMD_READ, addr) != 0) {
        uart_puts("CMD17 failed\n");
        return -1;
    }

    for (int i = 0; i < 128; i++) {

        int timeout = 100000;

        while ((*SD_HSTS & SD_HSTS_FIFO_EMPTY) && timeout--) {}

        if (timeout <= 0) {
            uart_puts("FIFO timeout\n");
            return -1;
        }

        unsigned int data = *SD_DATA;

        buffer[i * 4 + 0] = data & 0xFF;
        buffer[i * 4 + 1] = (data >> 8) & 0xFF;
        buffer[i * 4 + 2] = (data >> 16) & 0xFF;
        buffer[i * 4 + 3] = (data >> 24) & 0xFF;
    }

    return 0;
}
