#include "sd.h"
#include "uart.h"

#define SDHOST_BASE 0x3F202000

#define SD_CMD   ((volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SD_ARG   ((volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SD_TOUT  ((volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SD_CDIV  ((volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SD_RSP0  ((volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SD_RSP1  ((volatile unsigned int*)(SDHOST_BASE + 0x14))
#define SD_RSP2  ((volatile unsigned int*)(SDHOST_BASE + 0x18))
#define SD_RSP3  ((volatile unsigned int*)(SDHOST_BASE + 0x1C))
#define SD_HSTS  ((volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SD_VDD   ((volatile unsigned int*)(SDHOST_BASE + 0x30))
#define SD_EDM   ((volatile unsigned int*)(SDHOST_BASE + 0x34))
#define SD_DATA  ((volatile unsigned int*)(SDHOST_BASE + 0x40))

#define SD_HSTS_FIFO_EMPTY (1 << 1)
#define SD_HSTS_BUSY       (1 << 0)

#define SD_CMD_NEW_FLAG    (1 << 15)
#define SD_CMD_FAIL_FLAG   (1 << 14)
#define SD_CMD_BUSY        (1 << 7)
#define SD_CMD_RESPONSE    (1 << 6)
#define SD_CMD_LONGRESP    (1 << 5)
#define SD_CMD_WRITE      (1 << 4)
#define SD_CMD_READ       (1 << 3)

static unsigned int card_rca = 0;
static int is_sdhc = 0;

static void delay(int count) {
    while (count--) {
        asm volatile("nop");
    }
}

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
    uart_puts("Initializing SDHOST...\n");

    *SD_CDIV = 0x76; // slow clock
    delay(10000);

    // CMD0
    uart_puts("CMD0\n");
    if (sd_send_cmd(0, 0) != 0) return -1;

    // CMD8
    uart_puts("CMD8\n");
    if (sd_send_cmd(8 | SD_CMD_RESPONSE, 0x1AA) != 0) {
        uart_puts("CMD8 failed (maybe old card)\n");
    }

    // ACMD41 loop
    uart_puts("ACMD41 loop\n");
    int timeout = 100000;
    while (timeout--) {
        // CMD55
        if (sd_send_cmd(55 | SD_CMD_RESPONSE, 0) != 0) return -1;

        // ACMD41
        if (sd_send_cmd(41 | SD_CMD_RESPONSE, 0x40300000) != 0) return -1;

        if (*SD_RSP0 & (1 << 31)) {
            is_sdhc = (*SD_RSP0 & (1 << 30)) != 0;
            uart_puts("Card ready\n");
            break;
        }
    }

    if (timeout <= 0) {
        uart_puts("ACMD41 timeout\n");
        return -1;
    }

    // CMD2
    uart_puts("CMD2\n");
    if (sd_send_cmd(2 | SD_CMD_RESPONSE | SD_CMD_LONGRESP, 0) != 0) return -1;

    // CMD3
    uart_puts("CMD3\n");
    if (sd_send_cmd(3 | SD_CMD_RESPONSE, 0) != 0) return -1;

    card_rca = (*SD_RSP0 >> 16) & 0xFFFF;

    uart_puts("RCA: ");
    uart_puthex(card_rca);
    uart_puts("\n");

    // CMD7
    uart_puts("CMD7\n");
    if (sd_send_cmd(7 | SD_CMD_RESPONSE, card_rca << 16) != 0) return -1;

    // CMD16 (only needed for non SDHC)
    if (!is_sdhc) {
        uart_puts("CMD16\n");
        if (sd_send_cmd(16 | SD_CMD_RESPONSE, 512) != 0) return -1;
    }

    uart_puts("Card in TRANSFER state\n");

    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buffer) {

    unsigned int addr = is_sdhc ? lba : (lba * 512);

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
