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

#define INT_CMD_DONE     (1 << 0)
#define INT_ERROR_MASK   0xFFFF0000

// Command flags
#define CMD_RSPNS_NONE   (0 << 16)
#define CMD_RSPNS_136    (1 << 16)
#define CMD_RSPNS_48     (2 << 16)
#define CMD_CRCCHK_EN    (1 << 19)
#define CMD_IXCHK_EN     (1 << 20)

static void delay(int d) {
    while (d--) asm volatile("nop");
}

static void wait_ready(void) {
    // Wait until CMD and DATA lines are free
    while (EMMC_STATUS & ((1 << 0) | (1 << 1)));
}

static int wait_cmd_done(void) {
    int timeout = 2000000;

    while (timeout--) {
        unsigned int irpt = EMMC_INTERRUPT;

        if (irpt & INT_CMD_DONE) {
            EMMC_INTERRUPT = INT_CMD_DONE;
            return 0;
        }

        if (irpt & INT_ERROR_MASK) {
            uart_puts("CMD ERROR: ");
            uart_puthex(irpt);
            uart_puts("\n");
            return -1;
        }
    }

    uart_puts("CMD TIMEOUT\n");
    return -1;
}

static int emmc_cmd(unsigned int cmd, unsigned int arg) {
    wait_ready();

    // Clear interrupts
    EMMC_INTERRUPT = 0xFFFFFFFF;

    // Build command
    unsigned int cmdtm = (cmd << 24);

    switch (cmd) {
        case 0: // CMD0: no response
            cmdtm |= CMD_RSPNS_NONE;
            break;

        case 8: // CMD8
            cmdtm |= CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN;
            break;

        case 55:
        case 41:
            cmdtm |= CMD_RSPNS_48;
            break;

        default:
            cmdtm |= CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN;
            break;
    }

    EMMC_ARG1 = arg;
    EMMC_CMDTM = cmdtm;

    uart_puts("CMD ");
    uart_puthex(cmd);
    uart_puts("\n");

    return wait_cmd_done();
}

int emmc_init(void) {
    uart_puts("EMMC INIT START\n");

    // Reset controller
    EMMC_CONTROL1 |= (1 << 24);
    delay(10000);

    // Ensure reset cleared
    while (EMMC_CONTROL1 & (1 << 24));

    uart_puts("RESET DONE\n");

    // Set safe control
    EMMC_CONTROL0 = 0;

    // Set clock divider (~400kHz init)
    EMMC_CONTROL1 &= ~(0xFFF << 16);
    EMMC_CONTROL1 |= (250 << 16);

    // Enable internal clock
    EMMC_CONTROL1 |= (1 << 0);

    // Wait for stable clock
    while (!(EMMC_CONTROL1 & (1 << 1)));

    // Enable SD clock
    EMMC_CONTROL1 |= (1 << 2);

    uart_puts("CLOCK READY\n");

    // Enable interrupts
    EMMC_INTERRUPT = 0xFFFFFFFF;
    EMMC_IRPT_EN = 0xFFFFFFFF;

    // Small delay after clock
    delay(10000);

    // CMD0 (GO_IDLE_STATE)
    if (emmc_cmd(0, 0) != 0) {
        uart_puts("CMD0 FAIL\n");
        return -1;
    }

    uart_puts("CMD0 OK\n");

    // CMD8 (check voltage)
    if (emmc_cmd(8, 0x1AA) != 0) {
        uart_puts("CMD8 FAIL\n");
    } else {
        uart_puts("CMD8 OK\n");
    }

    return 0;
}
