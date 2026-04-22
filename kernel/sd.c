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

#define INT_CMD_DONE  (1 << 0)
#define INT_DATA_DONE (1 << 1)
#define INT_ERROR_MASK 0xFFFF0000

#define EMMC_CONTROL0 ((volatile unsigned int*)(EMMC_BASE + 0x28))

static void delay(int count) {
    while (count--) asm volatile("nop");
}

static int wait_cmd_done() {
    int timeout = 1000000;
    while (timeout--) {
        unsigned int irpt = *EMMC_INTERRUPT;
        if (irpt & INT_CMD_DONE) {
            *EMMC_INTERRUPT = INT_CMD_DONE;
            return 0;
        }
        if (irpt & INT_ERROR_MASK) {
            uart_puts("CMD ERR: ");
            uart_puthex(irpt);
            uart_puts("\n");
            return -1;
        }
    }
    uart_puts("CMD TIMEOUT\n");
    return -1;
}

static int wait_data_done() {
    int timeout = 1000000;
    while (timeout--) {
        unsigned int irpt = *EMMC_INTERRUPT;
        if (irpt & INT_DATA_DONE) {
            *EMMC_INTERRUPT = INT_DATA_DONE;
            return 0;
        }
        if (irpt & INT_ERROR_MASK) {
            uart_puts("DATA ERR: ");
            uart_puthex(irpt);
            uart_puts("\n");
            return -1;
        }
    }
    uart_puts("DATA TIMEOUT\n");
    return -1;
}

static int emmc_cmd(unsigned int cmd_idx, unsigned int arg, unsigned int flags) {

    while (*EMMC_STATUS & ((1 << 0) | (1 << 1))); // CMD inhibit

    *EMMC_INTERRUPT = *EMMC_INTERRUPT;

    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = (cmd_idx << 24) | flags;

    return wait_cmd_done();
}

int sd_init(void) {
    uart_puts("EMMC INIT START\n");

    // Reset controller
    *EMMC_CONTROL1 |= (1 << 24);
    delay(10000);

    *EMMC_CONTROL0 = 0x0;

    *EMMC_CONTROL1 &= ~(1 << 2);

    unsigned int divisor = 250;

    *EMMC_CONTROL1 &= ~(0xFFF << 16);

    *EMMC_CONTROL1 |= (divisor << 16);

    *EMMC_CONTROL1 |= (1 << 0);

    while(!(*EMMC_CONTROL1 & (1 << 1)));

    *EMMC_CONTROL1 |= (1 << 2);

    
    delay(10000);

    // Enable interrupts
    *EMMC_INTERRUPT = *EMMC_INTERRUPT;
    *EMMC_IRPT_EN = 0xFFFFFFFF;

    // CMD0
    if (emmc_cmd(0, 0, 0) != 0) return -1;

    // CMD8
    if (emmc_cmd(8, 0x1AA, (1 << 16)) != 0) {
        uart_puts("CMD8 fail\n");
    }

    // ACMD41 loop
    for (int i = 0; i < 10000; i++) {
        emmc_cmd(55, 0, (1 << 16));
        emmc_cmd(41, 0x40300000, (1 << 16));

        if (*EMMC_RESP0 & (1 << 31)) {
            uart_puts("CARD READY\n");
            break;
        }
    }

    // CMD2
    emmc_cmd(2, 0, (1 << 16));

    // CMD3
    emmc_cmd(3, 0, (1 << 16));

    unsigned int rca = (*EMMC_RESP0 >> 16);

    // CMD7
    emmc_cmd(7, rca << 16, (1 << 16));

    // CMD16
    emmc_cmd(16, 512, (1 << 16));

    uart_puts("SD READY\n");
    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buf) {

    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    if (emmc_cmd(17, lba, (1 << 16) | (1 << 21)) != 0) {
        uart_puts("CMD17 FAIL\n");
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
