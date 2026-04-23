#include "uart.h"

#define SDHOST_BASE 0x3F202000

#define SDCMD   (*(volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SDARG   (*(volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SDTOUT  (*(volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SDCDIV  (*(volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SDRSP0  (*(volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SDHSTS  (*(volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SDVDD   (*(volatile unsigned int*)(SDHOST_BASE + 0x30))

#define SDCMD_NEW_FLAG 0x8000
#define SDCMD_FAIL_FLAG 0x4000

static void delay(int count) {
    while (count--) asm volatile("nop");
}

void sdhost_reset(void) {
    uart_puts("SDHOST RESET\n");

    SDVDD = 0;
    delay(10000);

    SDCMD = 0;
    SDARG = 0;
    SDTOUT = 0xF00000;
    SDCDIV = 0;
    SDHSTS = 0x7F8;

    delay(10000);

    SDVDD = 1;
    delay(10000);
}

int sdhost_cmd(unsigned int cmd, unsigned int arg) {
    int timeout;

    uart_puts("CMD ");
    uart_puthex(cmd);
    uart_puts("\n");

    // Wait until controller is free
    timeout = 1000000;
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--);
    if (!timeout) {
        uart_puts("CMD BUSY TIMEOUT\n");
        return -1;
    }

    // Clear errors
    SDHSTS = SDHSTS;

    SDARG = arg;
    SDCMD = cmd | SDCMD_NEW_FLAG;

    // Wait for completion
    timeout = 1000000;
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--);
    if (!timeout) {
        uart_puts("CMD TIMEOUT\n");
        return -1;
    }

    if (SDCMD & SDCMD_FAIL_FLAG) {
        uart_puts("CMD FAIL\n");
        return -1;
    }

    uart_puts("CMD OK\n");
    return 0;
}
