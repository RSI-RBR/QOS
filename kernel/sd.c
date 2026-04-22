#include "uart.h"
#include "sd.h"

#define SDHOST_BASE 0x3F202000

#define SDHOST_CMD     (*(volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SDHOST_ARG     (*(volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SDHOST_TOUT    (*(volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SDHOST_CDIV    (*(volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SDHOST_RSP0    (*(volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SDHOST_HSTS    (*(volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SDHOST_VDD     (*(volatile unsigned int*)(SDHOST_BASE + 0x30))
#define SDHOST_EDM     (*(volatile unsigned int*)(SDHOST_BASE + 0x34))
#define SDHOST_HCFG    (*(volatile unsigned int*)(SDHOST_BASE + 0x38))
#define SDHOST_HBCT    (*(volatile unsigned int*)(SDHOST_BASE + 0x3C))
#define SDHOST_DATA    (*(volatile unsigned int*)(SDHOST_BASE + 0x40))
#define SDHOST_HBLC    (*(volatile unsigned int*)(SDHOST_BASE + 0x50))

#define SDHOST_HSTS_CMD_DONE (1 << 0)
#define SDHOST_HSTS_DATA_DONE (1 << 1)
#define SDHOST_HSTS_ERROR_MASK 0xFFFF0000

static void delay(int d) {
    while (d--) asm volatile("nop");
}

static void sdhost_wait_ready(void) {
    int timeout = 1000000;
    while ((SDHOST_CMD & (1 << 7)) && timeout--) {
        // wait for command not busy
    }
}

static int sdhost_send_cmd(unsigned int cmd, unsigned int arg) {
    sdhost_wait_ready();

    SDHOST_ARG = arg;
    SDHOST_CMD = cmd;

    int timeout = 1000000;

    while (timeout--) {
        if (SDHOST_HSTS & SDHOST_HSTS_CMD_DONE) {
            SDHOST_HSTS = SDHOST_HSTS_CMD_DONE;
            return 0;
        }

        if (SDHOST_HSTS & SDHOST_HSTS_ERROR_MASK) {
            uart_puts("SDHOST CMD ERROR: ");
            uart_puthex(SDHOST_HSTS);
            uart_puts("\n");
            return -1;
        }
    }

    uart_puts("SDHOST CMD TIMEOUT\n");
    return -1;
}

int sd_init(void) {
    uart_puts("SDHOST INIT START\n");

    // Power on
    SDHOST_VDD = 1;
    delay(10000);

    // Set slow clock (~400kHz)
    SDHOST_CDIV = 0x00000FA0;  // safe low speed

    // Timeout
    SDHOST_TOUT = 0x00FFFFFF;

    // Config: enable SD, 1-bit mode
    SDHOST_HCFG = (1 << 0);

    delay(10000);

    // CMD0 (reset)
    if (sdhost_send_cmd(0, 0) != 0) return -1;
    uart_puts("CMD0 OK\n");

    // CMD8 (voltage check)
    if (sdhost_send_cmd(8, 0x1AA) != 0) {
        uart_puts("CMD8 FAIL\n");
    } else {
        uart_puts("CMD8 OK\n");
    }

    // ACMD41 loop
    for (int i = 0; i < 10000; i++) {
        sdhost_send_cmd(55, 0);
        sdhost_send_cmd(41, 0x40300000);

        if (SDHOST_RSP0 & (1 << 31)) {
            uart_puts("CARD READY\n");
            break;
        }
    }

    uart_puts("SDHOST INIT DONE\n");
    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buf) {
    SDHOST_HBCT = 512;  // block size
    SDHOST_HBLC = 1;    // block count

    if (sdhost_send_cmd(17, lba) != 0) {
        uart_puts("CMD17 FAIL\n");
        return -1;
    }

    int timeout = 1000000;

    for (int i = 0; i < 128; i++) {
        while (!(SDHOST_HSTS & SDHOST_HSTS_DATA_DONE) && timeout--) {
            // wait for data
        }

        unsigned int d = SDHOST_DATA;

        buf[i*4+0] = d & 0xFF;
        buf[i*4+1] = (d >> 8) & 0xFF;
        buf[i*4+2] = (d >> 16) & 0xFF;
        buf[i*4+3] = (d >> 24) & 0xFF;
    }

    SDHOST_HSTS = SDHOST_HSTS_DATA_DONE;

    return 0;
}
