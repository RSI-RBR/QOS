#include "uart.h"

#define EMMC_BASE 0x3F300000

#define EMMC_ARG1        (*(volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM       (*(volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0       (*(volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_STATUS      (*(volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CONTROL0    (*(volatile unsigned int*)(EMMC_BASE + 0x28))
#define EMMC_CONTROL1    (*(volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT   (*(volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_IRPT_EN     (*(volatile unsigned int*)(EMMC_BASE + 0x38))

#define INT_CMD_DONE (1 << 0)

static void delay(int d) {
    while (d--) asm volatile("nop");
}

static int wait_cmd_done(void) {
    int timeout = 1000000;
    while (timeout--) {
        if (EMMC_INTERRUPT & INT_CMD_DONE) {
            EMMC_INTERRUPT = INT_CMD_DONE;
            return 0;
        }
    }
    uart_puts("CMD TIMEOUT\n");
    return -1;
}

static int emmc_cmd(unsigned int cmd, unsigned int arg, unsigned int flags) {
    while (EMMC_STATUS & ((1 << 0) | (1 << 1)));

    EMMC_INTERRUPT = 0xFFFFFFFF;

    EMMC_ARG1 = arg;
    EMMC_CMDTM = (cmd << 24) | flags;

    return wait_cmd_done();
}

int emmc_init(void) {
    uart_puts("EMMC INIT START\n");

    // Reset controller
    EMMC_CONTROL1 |= (1 << 24);
    delay(10000);

    // Set safe control
    EMMC_CONTROL0 = 0;

    // Setup clock divider (~400kHz)
    EMMC_CONTROL1 &= ~(0xFFF << 16);
    EMMC_CONTROL1 |= (250 << 16);

    // Enable internal clock
    EMMC_CONTROL1 |= (1 << 0);

    while (!(EMMC_CONTROL1 & (1 << 1)));

    // Enable SD clock
    EMMC_CONTROL1 |= (1 << 2);

    delay(10000);

    // Enable interrupts
    EMMC_INTERRUPT = 0xFFFFFFFF;
    EMMC_IRPT_EN = 0xFFFFFFFF;

    // CMD0
    if (emmc_cmd(0, 0, 0) != 0) return -1;

    uart_puts("CMD0 OK\n");

    // CMD8 (required for modern cards)
    if (emmc_cmd(8, 0x1AA, (1 << 16) | (1 << 19) | (1 << 20)) != 0) {
        uart_puts("CMD8 FAIL\n");
    } else {
        uart_puts("CMD8 OK\n");
    }

    return 0;
}
