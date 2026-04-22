#include "sd.h"
#include "uart.h"

#define EMMC_BASE 0x3F300000

#define EMMC_ARG1     ((volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM    ((volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0    ((volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_DATA     ((volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS   ((volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CONTROL1 ((volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT ((volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_IRPT_EN  ((volatile unsigned int*)(EMMC_BASE + 0x38))
#define EMMC_BLKSIZECNT ((volatile unsigned int*)(EMMC_BASE + 0x04))

#define CMD_DONE (1 << 0)
#define DATA_DONE (1 << 1)

static void delay(int d) {
    while (d--) asm volatile("nop");
}

static int wait_cmd_done() {
    int timeout = 1000000;
    while (timeout--) {
        if (*EMMC_INTERRUPT & CMD_DONE) {
            *EMMC_INTERRUPT = CMD_DONE;
            return 0;
        }
    }
    uart_puts("CMD timeout\n");
    return -1;
}

static int wait_data_done() {
    int timeout = 1000000;
    while (timeout--) {
        if (*EMMC_INTERRUPT & DATA_DONE) {
            *EMMC_INTERRUPT = DATA_DONE;
            return 0;
        }
    }
    uart_puts("DATA timeout\n");
    return -1;
}

static int emmc_cmd(unsigned int cmd, unsigned int arg) {
    while (*EMMC_STATUS & (1 << 0));

    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = cmd;

    return wait_cmd_done();
}

int sd_init(void) {
    uart_puts("EMMC init...\n");

    // Reset controller
    *EMMC_CONTROL1 |= (1 << 24);
    delay(10000);

    // Enable interrupts
    *EMMC_INTERRUPT = 0xFFFFFFFF;
    *EMMC_IRPT_EN = 0xFFFFFFFF;

    // Basic clock enable
    *EMMC_CONTROL1 |= (1 << 0);
    delay(10000);

    // CMD0
    if (emmc_cmd(0, 0) != 0) return -1;

    // CMD8
    if (emmc_cmd((8 << 24) | (1 << 16), 0x1AA) != 0) {
        uart_puts("CMD8 fail\n");
    }

    // ACMD41 loop
    for (int i = 0; i < 10000; i++) {
        if (emmc_cmd((55 << 24) | (1 << 16), 0) != 0) return -1;
        if (emmc_cmd((41 << 24) | (1 << 16), 0x40300000) != 0) return -1;

        if (*EMMC_RESP0 & (1 << 31)) {
            uart_puts("Card ready\n");
            break;
        }
    }

    // CMD2
    if (emmc_cmd((2 << 24) | (1 << 16), 0) != 0) return -1;

    // CMD3
    if (emmc_cmd((3 << 24) | (1 << 16), 0) != 0) return -1;

    unsigned int rca = (*EMMC_RESP0 >> 16);

    // CMD7
    if (emmc_cmd((7 << 24) | (1 << 16), rca << 16) != 0) return -1;

    // Set block size
    if (emmc_cmd((16 << 24) | (1 << 16), 512) != 0) return -1;

    uart_puts("SD ready\n");
    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buf) {

    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    if (emmc_cmd((17 << 24) | (1 << 16) | (1 << 21), lba) != 0) {
        uart_puts("CMD17 fail\n");
        return -1;
    }

    if (wait_data_done() != 0) return -1;

    for (int i = 0; i < 128; i++) {
        unsigned int d = *EMMC_DATA;
        buf[i*4+0] = d & 0xFF;
        buf[i*4+1] = (d >> 8) & 0xFF;
        buf[i*4+2] = (d >> 16) & 0xFF;
        buf[i*4+3] = (d >> 24) & 0xFF;
    }

    return 0;
}
