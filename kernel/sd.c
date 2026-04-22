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
#define SD_HCFG  ((volatile unsigned int*)(SDHOST_BASE + 0x38))
#define SD_DATA  ((volatile unsigned int*)(SDHOST_BASE + 0x40))

#define SD_CMD_START   (1u << 31)
#define SD_CMD_RESPONSE (1u << 6)
#define SD_CMD_LONGRESP (1u << 7)
#define SD_CMD_WRITE    (1u << 10)
#define SD_CMD_READ     (1u << 9)

#define SD_HSTS_FIFO_EMPTY (1 << 1)
#define SD_HSTS_BUSY (1 << 0)
#define SD_HSTS_DATA_FLAG (1 << 4)


static int is_sdhc = 0;

static void delay(int x) {
    for (volatile int i = 0; i < x * 1000; i++);
}

static void sd_send_cmd(unsigned int cmd, unsigned int arg) {
    *SD_ARG = arg;
    *SD_CMD = cmd | SD_CMD_START;
}

static int sd_wait_cmd_done() {
    int timeout = 1000000;

    while (timeout--) {
        if (!(*SD_CMD & SD_CMD_START)) {
            return 0;
        }
    }

    uart_puts("SDHOST CMD timeout\n");
    return -1;
}

int sd_init(void) {
    uart_puts("SDHOST init\n");

    // power on
    *SD_VDD = 0x1;
    delay(100);

    // slow clock for init
    *SD_CDIV = 400;

    *SD_HCFG = 0;

    // CMD0 reset
    sd_send_cmd(0, 0);
    sd_wait_cmd_done();
    uart_puts("CMD0 OK\n");

    // CMD8 (voltage check)
    sd_send_cmd(8 | SD_CMD_RESPONSE, 0x1AA);
    sd_wait_cmd_done();

    uart_puts("CMD8 OK\n");

    // ACMD41 init loop
    for (int i = 0; i < 1000; i++) {
        sd_send_cmd(55 | SD_CMD_RESPONSE, 0);
        sd_wait_cmd_done();

        sd_send_cmd(41 | SD_CMD_RESPONSE, 0x40300000);
        sd_wait_cmd_done();

        unsigned int r = *SD_RSP0;

        if (r & (1u << 31)) {
            uart_puts("Card ready\n");

            if (r & (1u << 30)) {
                is_sdhc = 1;
                uart_puts("SDHC detected\n");
            }

            break;
        }

        delay(10);
    }

    uart_puts("SDHOST init done\n");
    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buffer) {
    unsigned int addr = is_sdhc ? lba : (lba * 512);

    sd_send_cmd(17 | SD_CMD_RESPONSE | SD_CMD_READ, addr);
    sd_wait_cmd_done();

    // IMPORTANT: SDHOST FIFO read loop
    for (int i = 0; i < 128; i++) {
        int timeout = 100000;

        while ((*SD_HSTS & SD_HSTS_FIFO_EMPTY) && timeout--){
            // wait until fifo has data.
        }

        if (timeout <= 0){
            uart_puts("FIFO timeout.\n");
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
