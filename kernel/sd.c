#include "sd.h"
#include "uart.h"

#define EMMC_BASE 0x3F300000

#define EMMC_ARG2     ((volatile unsigned int*)(EMMC_BASE + 0x00))
#define EMMC_BLKSIZECNT ((volatile unsigned int*)(EMMC_BASE + 0x04))
#define EMMC_ARG1     ((volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM    ((volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0    ((volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_RESP1    ((volatile unsigned int*)(EMMC_BASE + 0x14))
#define EMMC_RESP2    ((volatile unsigned int*)(EMMC_BASE + 0x18))
#define EMMC_RESP3    ((volatile unsigned int*)(EMMC_BASE + 0x1C))
#define EMMC_DATA     ((volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS   ((volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CONTROL0 ((volatile unsigned int*)(EMMC_BASE + 0x28))
#define EMMC_CONTROL1 ((volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT ((volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_IRPT_MASK ((volatile unsigned int*)(EMMC_BASE + 0x34))
#define EMMC_IRPT_EN  ((volatile unsigned int*)(EMMC_BASE + 0x38))

// CONTROL1 bits
#define EMMC_CONTROL1_CLK_INTLEN  (1 << 0)
#define EMMC_CONTROL1_CLK_STABLE  (1 << 1)
#define EMMC_CONTROL1_CLK_EN      (1 << 2)
#define EMMC_CONTROL1_RESET_HC    (1 << 24)

// Interrupt bits
#define EMMC_INT_CMD_DONE    (1 << 0)
#define EMMC_INT_DATA_DONE   (1 << 1)
#define EMMC_INT_CMD_TIMEOUT (1 << 16)
#define EMMC_INT_CMD_CRC_ERR (1 << 17)
#define EMMC_INT_CMD_IDX_ERR (1 << 19)
#define EMMC_INT_DATA_TIMEOUT (1 << 20)

// Command response types
#define EMMC_CMD_RESP_NONE    (0 << 16)
#define EMMC_CMD_RESP_136     (1 << 16)
#define EMMC_CMD_RESP_48      (2 << 16)
#define EMMC_CMD_RESP_48B     (3 << 16)
#define EMMC_CMD_CRC_CHECK    (1 << 19)
#define EMMC_CMD_IDX_CHECK    (1 << 20)
#define EMMC_CMD_DATA         (1 << 21)

static void delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 1000; i++) {}
}

// Wait for command completion
static int wait_cmd_done() {
    int timeout = 1000000;

    while (timeout--) {
        unsigned int status = *EMMC_INTERRUPT;

        if (status & EMMC_INT_CMD_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_CMD_DONE;
            return 0;
        }

        if (status & (EMMC_INT_CMD_TIMEOUT | EMMC_INT_CMD_CRC_ERR | EMMC_INT_CMD_IDX_ERR)) {
            uart_puts("CMD ERROR: ");
            uart_puthex(status);
            uart_puts("\n");
            *EMMC_INTERRUPT = status;
            return -1;
        }
    }

    uart_puts("CMD TIMEOUT\n");
    return -1;
}

// Send command
static int emmc_cmd(unsigned int cmd_idx, unsigned int arg, unsigned int resp) {
    // Wait until command line free
    unsigned int timeout = 100000;
    while ((*EMMC_STATUS & (1 << 0)) && timeout--) {}

    if (timeout == 0) {
        uart_puts("CMD line busy\n");
        return -1;
    }

    // Clear interrupts BEFORE command
    *EMMC_INTERRUPT = 0xFFFFFFFF;

    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = (cmd_idx << 24) | resp;

    // Special case: CMD0 (no response)
    if (cmd_idx == 0) {
        delay_ms(10);
        return 0;
    }

    return wait_cmd_done();
}

// Read block (not usable yet until full init complete)
int sd_read_block(unsigned int lba, unsigned char *buffer) {
    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    int timeout = 100000;
    while ((*EMMC_STATUS & (1 << 1)) && timeout--) {}

    if (timeout == 0) {
        uart_puts("DATA line busy\n");
        return -1;
    }

    if (emmc_cmd(17, lba, EMMC_CMD_RESP_48 | EMMC_CMD_DATA) != 0) {
        uart_puts("CMD17 failed\n");
        return -1;
    }

    // Wait for data done
    timeout = 1000000;
    while (timeout--) {
        if (*EMMC_INTERRUPT & EMMC_INT_DATA_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_DATA_DONE;
            break;
        }
    }

    if (timeout <= 0) {
        uart_puts("DATA timeout\n");
        return -1;
    }

    for (int i = 0; i < 128; i++) {
        unsigned int data = *EMMC_DATA;
        buffer[i*4+0] = data & 0xFF;
        buffer[i*4+1] = (data >> 8) & 0xFF;
        buffer[i*4+2] = (data >> 16) & 0xFF;
        buffer[i*4+3] = (data >> 24) & 0xFF;
    }

    return 0;
}

// SD initialization (minimal stage)
int sd_init(void) {
    uart_puts("Initializing SD card...\n");

    // --- RESET ---
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC;

    int timeout = 100000;
    while ((*EMMC_CONTROL1 & EMMC_CONTROL1_RESET_HC) && timeout--) {}

    if (timeout <= 0) {
        uart_puts("Reset timeout\n");
        return -1;
    }

    uart_puts("Reset complete\n");

    // Clear interrupts
    *EMMC_INTERRUPT = 0xFFFFFFFF;
    *EMMC_IRPT_EN = 0xFFFFFFFF;

    // --- CLOCK SETUP ---
    unsigned int control = *EMMC_CONTROL1 & ~0xFFF;
    *EMMC_CONTROL1 = control | 240;

    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN;

    timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout--) {}

    if (timeout <= 0) {
        uart_puts("Clock failed\n");
        return -1;
    }

    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN;

    uart_puts("Clock stable\n");

    delay_ms(100);

    uart_puts("CONTROL1: ");
    uart_puthex(*EMMC_CONTROL1);
    uart_puts("\n");

    // --- CMD0 ---
    uart_puts("Sending CMD0...\n");

    if (emmc_cmd(0, 0, EMMC_CMD_RESP_NONE) != 0) {
        uart_puts("CMD0 failed\n");
        return -1;
    }

    uart_puts("CMD0 OK\n");

    uart_puts("Basic init complete (NOT ready for reads yet)\n");

    return 0;
}
