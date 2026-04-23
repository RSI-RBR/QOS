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

#define SDCMD_NO_RESPONSE 0x400
#define SDCMD_LONG_RESPONSE 0x200

unsigned int sdhost_get_resp(void){
    return SDRSP0;
}

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
    SDCDIV = 250;
    SDHSTS = 0x7F8;

    delay(10000);

    SDVDD = 1;
    delay(10000);
}

int sdhost_cmd(unsigned int cmd, unsigned int arg, unsigned int flags) {
    int timeout;

    uart_puts("CMD ");
    uart_puthex(cmd);
    uart_puts("\n");

    // Wait until controller free
    timeout = 1000000;
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--);
    if (!timeout) {
        uart_puts("CMD BUSY TIMEOUT\n");
        return -1;
    }

    // Clear errors
    SDHSTS = 0x7F8;

    unsigned int sdcmd = cmd;

    if (flags == 0) {
        sdcmd |= SDCMD_NO_RESPONSE;
    } else {
        if (flags == 136)
            sdcmd |= SDCMD_LONG_RESPONSE;
    }

    SDARG = arg;
    SDCMD = sdcmd | SDCMD_NEW_FLAG;

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

int sdhost_init_card(void) {
    uart_puts("SD INIT START\n");

    // CMD0 (reset)
    if (sdhost_cmd(0, 0, 0) != 0) return -1;

    // CMD8 (check voltage)
    if (sdhost_cmd(8, 0x1AA, 1) != 0) {
        uart_puts("CMD8 failed (maybe old card)\n");
    } else {
        uart_puts("CMD8 OK\n");
        uart_puts("RESP: ");
        uart_puthex(SDRSP0);
        uart_puts("\n");
    }

    unsigned int resp;

    int retries = 1000;

    do {
        if (sdhost_cmd(55, 0, 1) != 0){
            uart_puts("CMD55 FAIL\n");
            return -1;
        }
        uart_puts("CMD55 RESP = ");
        uart_puthex(SDRSP0);
        uart_puts("\n");

        if (sdhost_cmd(41, 0x40300000, 1) != 0){
            uart_puts("ACMD41 FAIL\n");
            return -1;
        }

        resp = sdhost_get_resp();

        uart_puts("ACMD41 RESP= ");
        uart_puthex(resp);
        uart_puts("\n");

        if (resp & 0x80000000) break;

        delay(10000);

    } while (-- retries);

    if (retries == 0){
        uart_puts("ACMD41 TIMEOUT\n");
        return -1;
    }

//    // ACMD41 loop
//    uart_puts("ACMD41 loop...\n");
//
//    for (int i = 0; i < 10000; i++) {
//        // CMD55 (APP_CMD)
//        if (sdhost_cmd(55, 0) != 0) return -1;
//
//        // ACMD41
//        if (sdhost_cmd(41, 0x40300000) != 0) return -1;
//
//        unsigned int resp = SDRSP0;
//
//        if (resp & (1 << 31)) {
//            uart_puts("CARD READY\n");
//            uart_puts("OCR: ");
//            uart_puthex(resp);
//            uart_puts("\n");
//            return 0;
//        }
//    }
//
//    uart_puts("ACMD41 TIMEOUT\n");
    return -1;
}
