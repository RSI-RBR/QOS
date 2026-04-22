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

// CONTROL1 Register Bit Definitions (SDHCI Standard)
#define EMMC_CONTROL1_CLK_INTLEN  (1 << 0)   // Internal Clock Enable (bit 0)
#define EMMC_CONTROL1_CLK_STABLE  (1 << 1)   // Internal Clock Stable (bit 1) - READ ONLY
#define EMMC_CONTROL1_CLK_EN      (1 << 2)   // SD Clock Enable (bit 2)
#define EMMC_CONTROL1_RESET_HC    (1 << 24)  // Reset host circuit
#define EMMC_CONTROL1_RESET_CMD   (1 << 25)  // Reset command circuit
#define EMMC_CONTROL1_RESET_DAT   (1 << 26)  // Reset data circuit

// Interrupt Register Bits
#define EMMC_INT_CMD_DONE    (1 << 0)   // Command complete
#define EMMC_INT_DATA_DONE   (1 << 1)   // Data complete
#define EMMC_INT_BLOCK_GAP   (1 << 2)   // Block gap
#define EMMC_INT_WRITE_RDY   (1 << 4)   // Write buffer ready
#define EMMC_INT_READ_RDY    (1 << 5)   // Read buffer ready
#define EMMC_INT_ERR         (1 << 15)  // Error interrupt
#define EMMC_INT_CMD_TIMEOUT (1 << 16)  // Command timeout error
#define EMMC_INT_CMD_CRC_ERR (1 << 17)  // Command CRC error
#define EMMC_INT_CMD_END_BIT (1 << 18)  // Command end bit error
#define EMMC_INT_CMD_IDX_ERR (1 << 19)  // Command index error
#define EMMC_INT_DATA_TIMEOUT (1 << 20) // Data timeout error
#define EMMC_INT_DATA_CRC_ERR (1 << 21) // Data CRC error
#define EMMC_INT_DATA_END_BIT (1 << 22) // Data end bit error

// Command register bits
#define EMMC_CMD_RESP_NONE    (0 << 16)  // No response
#define EMMC_CMD_RESP_136     (1 << 16)  // 136-bit response
#define EMMC_CMD_RESP_48      (2 << 16)  // 48-bit response
#define EMMC_CMD_RESP_48B     (3 << 16)  // 48-bit response with busy
#define EMMC_CMD_CRC_CHECK    (1 << 19)  // Check response CRC
#define EMMC_CMD_IDX_CHECK    (1 << 20)  // Check response index
#define EMMC_CMD_DATA         (1 << 21)  // Data transfer expected

// STATUS register bits
#define EMMC_STATUS_CMD_INHIBIT  (1 << 0)   // Command inhibit
#define EMMC_STATUS_DAT_INHIBIT  (1 << 1)   // Data inhibit
#define EMMC_STATUS_DAT_ACTIVE   (1 << 2)   // Data line active

// Card RCA (Relative Card Address) - will be set during init
static unsigned int card_rca = 0;

static void delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 1000; i++);
}

static void debug_emmc_status() {
    uart_puts("STATUS: 0x");
    uart_puthex(*EMMC_STATUS);
    uart_puts("\n");
    uart_puts("INTERRUPT STATUS: 0x");
    uart_puthex(*EMMC_INTERRUPT);
    uart_puts("\n");
}

/**
 * Wait for command to complete or report errors on failure
 */
static int wait_cmd_done() {
    int timeout = 1000000;
    while (timeout > 0) {
        unsigned int status = *EMMC_INTERRUPT;
        if (status & EMMC_INT_CMD_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_CMD_DONE;  // Acknowledge interrupt
            return 0; // Success
        }

        if (status & (EMMC_INT_CMD_TIMEOUT | EMMC_INT_CMD_CRC_ERR | 
                      EMMC_INT_CMD_END_BIT | EMMC_INT_CMD_IDX_ERR)) {
            uart_puts("ERROR: Command error, INTERRUPT STATUS: ");
            uart_puthex(status);
            uart_puts("\n");
            *EMMC_INTERRUPT = status;  // Clear all error interrupts
            return -1; // Failure
        }
        timeout--;
    }
    uart_puts("ERROR: Command timeout\n");
    return -1; // Timeout
}

/**
 * Send an EMMC command to the card
 */
static int emmc_cmd(unsigned int cmd_idx, unsigned int arg, unsigned int resp) {
    // Wait until controller is ready to accept a new command
    unsigned int timeout = 100000;
    while ((*EMMC_STATUS & EMMC_STATUS_CMD_INHIBIT) && timeout > 0) {
        timeout--;
    }

    if (timeout <= 0) {
        uart_puts("ERROR: Controller still busy\n");
        return -1;
    }

    *EMMC_ARG1 = arg; // Set argument
    *EMMC_CMDTM = (cmd_idx << 24) | resp; // Command index + response type
    return wait_cmd_done(); // Wait for result
}

/**
 * Send CMD0 (GO_IDLE_STATE) - Resets the card to idle state
 */
static int emmc_send_cmd0() {
    return emmc_cmd(0, 0, EMMC_CMD_RESP_NONE); // CMD0 doesn't expect a response
}

/**
 * Send CMD2 (ALL_SEND_CID) - Asks the card to send its CID
 */
static int emmc_send_cmd2() {
    uart_puts("CMD2: GET CID\n");
    return emmc_cmd(2, 0, EMMC_CMD_RESP_136);
}

/**
 * Send CMD3 (SEND_RELATIVE_ADDR) - Obtain the card's RCA
 */
static int emmc_send_cmd3() {
    uart_puts("CMD3: GET RCA\n");
    if (emmc_cmd(3, 0, EMMC_CMD_RESP_48) != 0) {
        return -1;
    }
    // Parse RCA from the response
    card_rca = (*EMMC_RESP0 >> 16) & 0xFFFF;
    uart_puts("RCA: 0x");
    uart_puthex(card_rca);
    uart_puts("\n");
    return 0;
}

/**
 * High-level EMMC initialization
 */
int sd_init() {
    uart_puts("Initializing EMMC...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC; // Reset the host controller
    delay_ms(10); // Small delay

    // Clear interrupts
    *EMMC_INTERRUPT = 0xFFFFFFFF;

    // Set clock frequency (400kHz for initialization)
    unsigned int control = *EMMC_CONTROL1 & ~0xFFFF; // Clear clock divider
    *EMMC_CONTROL1 = control | 240; // Clock divider of 240 (~400kHz)
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN; // Enable internal clock

    // Wait for clock to stabilize
    int timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout > 0) {
        timeout--;
    }
    if (timeout <= 0) {
        uart_puts("ERROR: Clock stabilization timeout\n");
        return -1; // Failure
    }
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN; // Enable SD clock output
    delay_ms(10);

    // Send CMD0 to reset the SD card
    if (emmc_send_cmd0() != 0) {
        uart_puts("ERROR: CMD0 failed\n");
        return -1;
    }

    // Send CMD2 to get the CID
    if (emmc_send_cmd2() != 0) {
        uart_puts("ERROR: CMD2 failed\n");
        return -1;
    }

    // Send CMD3 to obtain the RCA
    if (emmc_send_cmd3() != 0) {
        uart_puts("ERROR: CMD3 failed\n");
        return -1;
    }

    uart_puts("EMMC initialization complete\n");
    return 0; // Success
}
