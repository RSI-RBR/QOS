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
#define EMMC_CMD_TYPE_NORMAL  (0 << 22)  // Normal command
#define EMMC_CMD_TYPE_SUSPEND (1 << 22)  // Suspend command
#define EMMC_CMD_TYPE_RESUME  (2 << 22)  // Resume command
#define EMMC_CMD_TYPE_ABORT   (3 << 22)  // Abort command

static void delay_ms(int ms){
    for (volatile int i = 0; i < ms * 1000; i++);
}

static int wait_cmd_done(){
    int timeout = 1000000;
    unsigned int status;
    
    while (timeout > 0){
        status = *EMMC_INTERRUPT;
        
        // Check for command complete
        if (status & EMMC_INT_CMD_DONE){
            *EMMC_INTERRUPT = EMMC_INT_CMD_DONE;  // Clear the interrupt
            return 0;
        }
        
        // Check for command errors
        if (status & (EMMC_INT_CMD_TIMEOUT | EMMC_INT_CMD_CRC_ERR | 
                      EMMC_INT_CMD_END_BIT | EMMC_INT_CMD_IDX_ERR)){
            uart_puts("ERROR: Command error - interrupt status: 0x");
            uart_puthex(status);
            uart_puts("\n");
            *EMMC_INTERRUPT = status;  // Clear all interrupts
            return -1;
        }
        
        timeout--;
    }
    
    uart_puts("ERROR: Command timeout - final interrupt status: 0x");
    status = *EMMC_INTERRUPT;
    uart_puthex(status);
    uart_puts("\n");
    return -1;
}

static int wait_data_done(){
    int timeout = 1000000;
    unsigned int status;
    
    while (timeout > 0){
        status = *EMMC_INTERRUPT;
        
        // Check for data complete
        if (status & EMMC_INT_DATA_DONE){
            *EMMC_INTERRUPT = EMMC_INT_DATA_DONE;  // Clear the interrupt
            return 0;
        }
        
        // Check for data errors
        if (status & (EMMC_INT_DATA_TIMEOUT | EMMC_INT_DATA_CRC_ERR | 
                      EMMC_INT_DATA_END_BIT)){
            uart_puts("ERROR: Data error - interrupt status: 0x");
            uart_puthex(status);
            uart_puts("\n");
            *EMMC_INTERRUPT = status;  // Clear all interrupts
            return -1;
        }
        
        timeout--;
    }
    
    uart_puts("ERROR: Data timeout\n");
    return -1;
}

/**
 * Send an EMMC command with proper response handling
 * cmd_index: Command index (0-63)
 * arg: Command argument
 * resp_type: Expected response type (EMMC_CMD_RESP_*)
 */
static int emmc_cmd(unsigned int cmd_index, unsigned int arg, unsigned int resp_type){
    // Wait for command line to be ready
    int timeout = 100000;
    while ((*EMMC_STATUS & (1 << 9)) && timeout > 0){  // CMD_INHIBIT bit
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Command line busy\n");
        return -1;
    }
    
    // Set argument
    *EMMC_ARG1 = arg;
    
    // Build command register value
    // Bits 31:24 = command index
    // Bits 23:16 = response type
    unsigned int cmd = (cmd_index << 24) | resp_type;
    
    // Set command
    *EMMC_CMDTM = cmd;
    
    return wait_cmd_done();
}

/**
 * Initialize the EMMC controller and SD card
 */
int sd_init(void){
    uart_puts("\n=== Initializing EMMC ===\n");

    // Step 1: Reset host controller
    uart_puts("Step 1: Resetting EMMC controller...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC;
    
    int timeout = 100000;
    while ((*EMMC_CONTROL1 & EMMC_CONTROL1_RESET_HC) && timeout > 0){
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Controller reset timeout\n");
        return -1;
    }
    uart_puts("  Reset complete\n");

    // Clear any pending interrupts after reset
    *EMMC_INTERRUPT = 0xFFFFFFFF;

    // Step 2: Set clock divider
    uart_puts("Step 2: Setting clock divider...\n");
    unsigned int control1 = *EMMC_CONTROL1;
    control1 &= ~(0xFFFF);
    control1 |= 240;              // Set divider to 240 for ~400kHz
    *EMMC_CONTROL1 = control1;
    uart_puts("  Divider set\n");

    // Step 3: Enable internal clock
    uart_puts("Step 3: Enabling internal clock...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN;
    
    // Wait for clock to stabilize
    timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout > 0){
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Clock stabilization timeout\n");
        return -1;
    }
    uart_puts("  Clock stable\n");

    // Step 4: Enable SD clock output
    uart_puts("Step 4: Enabling SD clock output...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN;
    delay_ms(10);
    uart_puts("  SD clock enabled\n");

    // Step 5: Enable interrupts
    uart_puts("Step 5: Enabling interrupts...\n");
    unsigned int int_en = EMMC_INT_CMD_DONE | EMMC_INT_DATA_DONE | 
                          EMMC_INT_CMD_TIMEOUT | EMMC_INT_CMD_CRC_ERR |
                          EMMC_INT_CMD_END_BIT | EMMC_INT_CMD_IDX_ERR |
                          EMMC_INT_DATA_TIMEOUT | EMMC_INT_DATA_CRC_ERR |
                          EMMC_INT_DATA_END_BIT;
    *EMMC_IRPT_EN = int_en;
    *EMMC_IRPT_MASK = int_en;
    *EMMC_INTERRUPT = 0xFFFFFFFF;  // Clear any pending interrupts
    uart_puts("  Interrupts enabled\n");

    // Step 6: Send CMD0 (reset)
    uart_puts("Step 6: Sending CMD0 (reset)...\n");
    if (emmc_cmd(0, 0, EMMC_CMD_RESP_NONE) != 0){
        uart_puts("  ERROR: CMD0 failed\n");
        return -1;
    }
    uart_puts("  CMD0 OK\n");
    delay_ms(1);

    // Step 7: Send CMD8 (voltage check)
    uart_puts("Step 7: Sending CMD8 (voltage check)...\n");
    if (emmc_cmd(8, 0x1AA, EMMC_CMD_RESP_48) != 0){
        uart_puts("  ERROR: CMD8 failed\n");
        return -1;
    }
    uart_puts("  CMD8 OK, response: 0x");
    uart_puthex(*EMMC_RESP0);
    uart_puts("\n");
    delay_ms(1);

    // Step 8: ACMD41 loop - wait for card to be ready
    uart_puts("Step 8: Sending ACMD41 loop (init card)...\n");
    for (int i = 0; i < 1000; i++){
        // CMD55 (App command)
        if (emmc_cmd(55, 0, EMMC_CMD_RESP_48) != 0){
            uart_puts("  Attempt ");
            uart_puthex(i);
            uart_puts(": CMD55 error\n");
            delay_ms(1);
            continue;
        }
        
        // ACMD41 (SD send operation conditions)
        if (emmc_cmd(41, 0x40FF8000, EMMC_CMD_RESP_48) != 0){
            uart_puts("  Attempt ");
            uart_puthex(i);
            uart_puts(": ACMD41 error\n");
            delay_ms(1);
            continue;
        }

        // Check if card is ready (bit 31 of response = card ready)
        unsigned int resp = *EMMC_RESP0;
        if (resp & (1 << 31)){
            uart_puts("  Card ready after ");
            uart_puthex(i);
            uart_puts(" attempts!\n");
            uart_puts("=== EMMC Init Complete ===\n");
            return 0;
        }
        
        delay_ms(1);
    }

    uart_puts("ERROR: Card initialization failed - ACMD41 timeout\n");
    return -1;
}

int sd_read_block(unsigned int lba, unsigned char *buffer){
    // Set block size = 512, count = 1
    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    // CMD17 (read single block)
    if (emmc_cmd(17, lba, EMMC_CMD_RESP_48) != 0){
        uart_puts("ERROR: CMD17 failed\n");
        return -1;
    }

    if (wait_data_done() != 0){
        return -1;
    }

    for (int i = 0; i < 128; i++){
        unsigned int data = *EMMC_DATA;

        buffer[i*4+0] = data & 0xFF;
        buffer[i*4+1] = (data >> 8) & 0xFF;
        buffer[i*4+2] = (data >> 16) & 0xFF;
        buffer[i*4+3] = (data >> 24) & 0xFF;
    }

    return 0;
}

int sd_read_file(const char *path, void *buf, int max_size){
    (void)path;

    sd_read_blocks(100, buf, (max_size / 512));

    return max_size;
}

int sd_read_blocks(uint32_t lba, uint8_t *buffer, int count){
    for (int i = 0; i < count; i++){
        if (sd_read_block(lba + i, buffer + (i * 512)) != 0){
            uart_puts("ERROR: Failed to read block\n");
            return -1;
        }
    }

    return 0;
}
