#include "sd.h"
#include "uart.h"

#define EMMC_BASE 0x3F300000

#define EMMC_ARG1     ((volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM    ((volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0    ((volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_DATA     ((volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS   ((volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_INTERRUPT ((volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_BLKSIZECNT ((volatile unsigned int*)(EMMC_BASE + 0x04))

#define INT_CMD_DONE  (1 << 0)
#define INT_DATA_DONE (1 << 1)
#define INT_ERROR_MASK 0xFFFF0000

// --- Wait for command completion ---
static int wait_cmd_done() {
    int timeout = 1000000;

    while (timeout--) {
        unsigned int irpt = *EMMC_INTERRUPT;

        if (irpt & INT_CMD_DONE) {
            *EMMC_INTERRUPT = INT_CMD_DONE; // clear
            return 0;
        }

        if (irpt & INT_ERROR_MASK) {
            uart_puts("CMD error: ");
            uart_puthex(irpt);
            uart_puts("\n");
            *EMMC_INTERRUPT = irpt;
            return -1;
        }
    }

    uart_puts("CMD timeout\n");
    return -1;
}

// --- Wait for data completion ---
static int wait_data_done() {
    int timeout = 1000000;

    while (timeout--) {
        unsigned int irpt = *EMMC_INTERRUPT;

        if (irpt & INT_DATA_DONE) {
            *EMMC_INTERRUPT = INT_DATA_DONE;
            return 0;
        }

        if (irpt & INT_ERROR_MASK) {
            uart_puts("DATA error: ");
            uart_puthex(irpt);
            uart_puts("\n");
            *EMMC_INTERRUPT = irpt;
            return -1;
        }
    }

    uart_puts("DATA timeout\n");
    return -1;
}

// --- Send command WITHOUT reinitializing controller ---
static int emmc_cmd(unsigned int cmd_idx, unsigned int arg, unsigned int flags) {

    // Wait until command line free
    int timeout = 1000000;
    while ((*EMMC_STATUS & (1 << 0)) && timeout--) {}

    if (timeout <= 0) {
        uart_puts("CMD line busy\n");
        return -1;
    }

    // Clear interrupts BEFORE sending
    *EMMC_INTERRUPT = 0xFFFFFFFF;

    *EMMC_ARG1 = arg;

    // Build command
    unsigned int cmd =
        (cmd_idx << 24) |
        flags;

    *EMMC_CMDTM = cmd;

    return wait_cmd_done();
}

int sd_init(void) {
    uart_puts("SD using firmware-initialized controller\n");
    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buffer) {

    // Firmware already uses SDHC → block addressing
    unsigned int addr = lba;

    // Set block size = 512, count = 1
    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    // CMD17: READ_SINGLE_BLOCK
    if (emmc_cmd(
        17,
        addr,
        (1 << 16) | // response
        (1 << 21)   // data
    ) != 0) {
        uart_puts("CMD17 failed\n");
        return -1;
    }

    if (wait_data_done() != 0) {
        uart_puts("Read failed\n");
        return -1;
    }

    // Read FIFO
    for (int i = 0; i < 128; i++) {
        unsigned int data = *EMMC_DATA;

        buffer[i*4 + 0] = data & 0xFF;
        buffer[i*4 + 1] = (data >> 8) & 0xFF;
        buffer[i*4 + 2] = (data >> 16) & 0xFF;
        buffer[i*4 + 3] = (data >> 24) & 0xFF;
    }

    return 0;
}
