#include "sd.h"
#include "uart.h"

#define EMMC_BASE 0x3F300000

#define EMMC_ARG1     ((volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM    ((volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0    ((volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_RESP1    ((volatile unsigned int*)(EMMC_BASE + 0x14))
#define EMMC_RESP2    ((volatile unsigned int*)(EMMC_BASE + 0x18))
#define EMMC_RESP3    ((volatile unsigned int*)(EMMC_BASE + 0x1C))
#define EMMC_DATA     ((volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS   ((volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CONTROL1 ((volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT ((volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_IRPT_EN  ((volatile unsigned int*)(EMMC_BASE + 0x38))

#define EMMC_CONTROL1_CLK_INTLEN  (1 << 0)
#define EMMC_CONTROL1_CLK_STABLE  (1 << 1)
#define EMMC_CONTROL1_CLK_EN      (1 << 2)
#define EMMC_CONTROL1_RESET_HC    (1 << 24)

#define EMMC_INT_CMD_DONE    (1 << 0)
#define EMMC_INT_CMD_TIMEOUT (1 << 16)

#define EMMC_CMD_RESP_NONE (0 << 16)
#define EMMC_CMD_RESP_48   (2 << 16)
#define EMMC_CMD_RESP_136  (1 << 16)

static unsigned int rca = 0;

static void delay(int x) {
    for (volatile int i = 0; i < x * 1000; i++);
}

static int wait_cmd_done() {
    int timeout = 1000000;
    while (timeout--) {
        unsigned int s = *EMMC_INTERRUPT;

        if (s & EMMC_INT_CMD_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_CMD_DONE;
            return 0;
        }

        if (s & EMMC_INT_CMD_TIMEOUT) {
            uart_puts("CMD TIMEOUT\n");
            return -1;
        }
    }
    uart_puts("CMD WAIT TIMEOUT\n");
    return -1;
}

static int cmd(unsigned int idx, unsigned int arg, unsigned int resp) {
    while (*EMMC_STATUS & 1); // wait cmd line free

    *EMMC_INTERRUPT = 0xFFFFFFFF;
    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = (idx << 24) | resp;

    if (idx == 0) {
        delay(10);
        return 0;
    }

    return wait_cmd_done();
}

int sd_init(void) {
    uart_puts("SD init start\n");

    // Reset
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC;
    int timeout = 100000;
    while ((*EMMC_CONTROL1 & EMMC_CONTROL1_RESET_HC) && timeout--);
    if (timeout <= 0) {
        uart_puts("Reset fail\n");
        return -1;
    }

    // Interrupts
    *EMMC_INTERRUPT = 0xFFFFFFFF;
    *EMMC_IRPT_EN = 0xFFFFFFFF;

    // Clock
    unsigned int c = *EMMC_CONTROL1 & ~0xFFF;
    *EMMC_CONTROL1 = c | 240;
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN;

    timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout--);
    if (timeout <= 0) {
        uart_puts("Clock fail\n");
        return -1;
    }

    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN;
    delay(100);

    uart_puts("CMD0\n");
    if (cmd(0, 0, EMMC_CMD_RESP_NONE)) return -1;

    uart_puts("CMD8\n");
    if (cmd(8, 0x1AA, EMMC_CMD_RESP_48)) return -1;

    unsigned int resp = *EMMC_RESP0;
    uart_puts("CMD8 resp: ");
    uart_puthex(resp);
    uart_puts("\n");

    // ACMD41 loop
    uart_puts("ACMD41 loop\n");
    for (int i = 0; i < 1000; i++) {
        cmd(55, 0, EMMC_CMD_RESP_48); // APP_CMD
        cmd(41, 0x40300000, EMMC_CMD_RESP_48);

        resp = *EMMC_RESP0;

        if (resp & (1 << 31)) {
            uart_puts("Card ready\n");
            break;
        }

        delay(10);
    }

    // CMD2
    uart_puts("CMD2\n");
    if (cmd(2, 0, EMMC_CMD_RESP_136)) return -1;

    // CMD3
    uart_puts("CMD3\n");
    if (cmd(3, 0, EMMC_CMD_RESP_48)) return -1;

    rca = *EMMC_RESP0 & 0xFFFF0000;

    uart_puts("RCA: ");
    uart_puthex(rca);
    uart_puts("\n");

    // CMD7 (select card)
    uart_puts("CMD7\n");
    if (cmd(7, rca, EMMC_CMD_RESP_48)) return -1;

    uart_puts("SD card selected\n");

    return 0;
}
