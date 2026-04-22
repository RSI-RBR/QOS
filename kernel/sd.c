/**
 * Function Name: sd_init
 * Description: Initialize the SD card via the EMMC controller
 * Uses the EMMC controller to initialize the SD card and set it to idle state.
 * Returns: 0 on success, -1 on failure.
 */

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

// CONTROL1 Register Bit Definitions
#define EMMC_CONTROL1_CLK_INTLEN  (1 << 0)
#define EMMC_CONTROL1_CLK_STABLE  (1 << 1)  // Clock Stable (read-only)
#define EMMC_CONTROL1_CLK_EN      (1 << 2)
#define EMMC_CONTROL1_RESET_HC    (1 << 24)

// Interrupt Register Bits
#define EMMC_INT_CMD_DONE    (1 << 0)
#define EMMC_INT_DATA_DONE   (1 << 1)
#define EMMC_INT_CMD_TIMEOUT (1 << 16)
#define EMMC_INT_CMD_CRC_ERR (1 << 17)
#define EMMC_INT_CMD_IDX_ERR (1 << 19)
#define EMMC_INT_DATA_TIMEOUT (1 << 20)

// Command Register Bits
#define EMMC_CMD_RESP_NONE    (0 << 16)
#define EMMC_CMD_RESP_136     (1 << 16)  // 136-bit response
#define EMMC_CMD_RESP_48      (2 << 16)  // 48-bit response
#define EMMC_CMD_RESP_48B     (3 << 16)  // 48-bit response with busy
#define EMMC_CMD_CRC_CHECK    (1 << 19)
#define EMMC_CMD_IDX_CHECK    (1 << 20)
#define EMMC_CMD_DATA         (1 << 21)

// Card RCA will be set during init
static unsigned int card_rca = 0;

static void delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 1000; i++) {}
}

/**
 * Wait for command to complete or report failure with enhanced debugging
 */
static int wait_cmd_done() {
    int timeout = 1000000;
    int iterations = 0;
    while (timeout > 0) {
        unsigned int status = *EMMC_INTERRUPT;
        iterations++;

        if (status & EMMC_INT_CMD_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_CMD_DONE;  // Acknowledge interrupt
            uart_puts("CMD done after ");
            uart_puthex(iterations);
            uart_puts(" iterations\n");
            return 0; // Success
        }

        if (status & (EMMC_INT_CMD_TIMEOUT | EMMC_INT_CMD_CRC_ERR | EMMC_INT_CMD_IDX_ERR)) {
            uart_puts("ERROR at iteration ");
            uart_puthex(iterations);
            uart_puts(": STATUS=");
            uart_puthex(status);
            uart_puts("\n");
            *EMMC_INTERRUPT = status;  // Clear all error interrupts
            return -1; // Failure
        }
        timeout--;
    }

    uart_puts("ERROR: Timeout after ");
    uart_puthex(iterations);
    uart_puts(" iterations\n");
    return -1; // Timeout
}

static int wait_data_done() {
    int timeout = 1000000;
    while (timeout > 0) {
        unsigned int status = *EMMC_INTERRUPT;

        if (status & EMMC_INT_DATA_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_DATA_DONE;  // Acknowledge interrupt
            return 0; // Success
        }

        if (status & (EMMC_INT_DATA_TIMEOUT)) {
            uart_puts("ERROR: Data timeout, INTERRUPT STATUS: ");
            uart_puthex(status);
            uart_puts("\n");
            *EMMC_INTERRUPT = status;  // Clear all error interrupts
            return -1; // Failure
        }
        timeout--;
    }

    uart_puts("ERROR: Data transfer timeout\n");
    return -1; // Timeout
}

/**
 * Send an EMMC command to the card, with appropriate response type
 */
static int emmc_cmd(unsigned int cmd_idx, unsigned int arg, unsigned int resp) {
    // Wait for command line to be ready
    unsigned int timeout = 100000;
    while ((*EMMC_STATUS & (1 << 0)) && timeout > 0) {
        timeout--;
    }

    if (timeout == 0) {
        uart_puts("ERROR: Command line is still busy\n");
        return -1;
    }

    // Set command argument and send command
    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = (cmd_idx << 24) | resp;
    return wait_cmd_done();
}

/**
 * Read a single block (512 bytes) from the card
 */
int sd_read_block(unsigned int lba, unsigned char *buffer) {
    // Set block size (512 bytes) and block count (1)
    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    // Wait for data line to be ready
    int timeout = 100000;
    while ((*EMMC_STATUS & (1 << 1)) && timeout > 0) { 
        timeout--;
    }

    if (timeout == 0) {
        uart_puts("ERROR: Data line is busy\n");
        return -1;
    }

    // Send CMD17 (READ_SINGLE_BLOCK)
    if (emmc_cmd(17, lba, EMMC_CMD_RESP_48 | EMMC_CMD_DATA) != 0) {
        uart_puts("ERROR: CMD17 failed\n");
        return -1;
    }

    // Wait for data transfer to complete
    if (wait_data_done() != 0) {
        return -1;
    }

    // Read block data
    for (int i = 0; i < 128; i++) {
        unsigned int data = *EMMC_DATA;
        buffer[i * 4 + 0] = data & 0xFF;
        buffer[i * 4 + 1] = (data >> 8) & 0xFF;
        buffer[i * 4 + 2] = (data >> 16) & 0xFF;
        buffer[i * 4 + 3] = (data >> 24) & 0xFF;
    }

    return 0;
}

/**
 * Initialize the SD card via the EMMC controller
 */
int sd_init(void) {
    uart_puts("Initializing SD card...\n");

    // Step 1: Reset the host controller
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC;
    delay_ms(10);

    // Clear any pending interrupts
    *EMMC_INTERRUPT = 0xFFFFFFFF;
    
    // CRITICAL: Enable interrupt signals
    *EMMC_IRPT_EN = 0xFFFFFFFF;

    // Step 2: Set clock frequency (~400kHz for initialization)
    unsigned int control = *EMMC_CONTROL1 & ~0xFFF;
    *EMMC_CONTROL1 = control | 240; // Set clock divider
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN; // Enable internal clock

    // Wait for the clock to stabilize
    int timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout > 0) {
        timeout--;
    }

    if (timeout == 0) {
        uart_puts("ERROR: Clock stabilization timeout\n");
        return -1;
    }

    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN; // Enable SD clock output
    
    // Extended delay after clock enable - required for card initialization
    delay_ms(100);

    uart_puts("About to send CMD0, CONTROL1=");
    uart_puthex(*EMMC_CONTROL1);
    uart_puts(", IRPT_EN=");
    uart_puthex(*EMMC_IRPT_EN);
    uart_puts("\n");

    // Step 3: Send CMD0 to reset the card to idle
    if (emmc_cmd(0, 0, EMMC_CMD_RESP_NONE) != 0) {
        uart_puts("ERROR: CMD0 failed\n");
        return -1;
    }

    uart_puts("SD card initialized successfully\n");
    return 0;
}