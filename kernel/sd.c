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

static int wait_cmd_done(){
    int timeout = 1000000;
    while (!(*EMMC_INTERRUPT & 0x1) && timeout > 0){
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Command timeout\n");
        return -1;
    }
    
    *EMMC_INTERRUPT = 0x1;
    return 0;
}

static int wait_data_done(){
    int timeout = 1000000;
    while (!(*EMMC_INTERRUPT & 0x2) && timeout > 0){
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Data timeout\n");
        return -1;
    }
    
    *EMMC_INTERRUPT = 0x2;
    return 0;
}

static int emmc_cmd(unsigned int cmd, unsigned int arg){
    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = cmd;

    return wait_cmd_done();
}

/**
 * Initialize the EMMC controller and SD card
 * 
 * Proper initialization sequence:
 * 1. Reset the host controller
 * 2. Set clock divider for 400kHz (initialization frequency)
 * 3. Enable internal clock (bit 0) and wait for stable (bit 1)
 * 4. Enable SD clock output (bit 2)
 * 5. Send CMD0 (reset)
 * 6. Send CMD8 (voltage check)
 * 7. Loop ACMD41 until card is ready
 */
int sd_init(void){
    uart_puts("Initializing EMMC...\n");

    // Step 1: Reset host controller
    uart_puts("Resetting EMMC controller...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC;
    
    int timeout = 100000;
    while ((*EMMC_CONTROL1 & EMMC_CONTROL1_RESET_HC) && timeout > 0){
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Controller reset timeout\n");
        return -1;
    }

    // Step 2: Set clock divider
    // Raspberry Pi 3 EMMC clock is ~250MHz
    // For 400kHz initialization: divider = 250MHz / 400kHz / 2 ≈ 312 (or ~240 for safety)
    uart_puts("Setting clock divider...\n");
    unsigned int control1 = *EMMC_CONTROL1;
    control1 &= ~(0xFFFF);        // Clear existing divider and clock bits
    control1 |= 240;              // Set divider to 240
    *EMMC_CONTROL1 = control1;

    // Step 3: Enable internal clock (bit 0)
    uart_puts("Enabling internal clock...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN;  // Set bit 0 to enable internal clock
    
    // Wait for clock to stabilize (bit 1)
    timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout > 0){
        timeout--;
    }
    
    if (timeout <= 0){
        uart_puts("ERROR: Clock stabilization timeout\n");
        uart_puts("DEBUG: CONTROL1 register = 0x");
        unsigned int debug_val = *EMMC_CONTROL1;
        for (int i = 28; i >= 0; i -= 4){
            unsigned int nibble = (debug_val >> i) & 0xF;
            char hex_char = nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10);
            uart_putc(hex_char);
        }
        uart_puts("\n");
        return -1;
    }
    uart_puts("Clock stable\n");

    // Step 4: Enable SD clock output (bit 2)
    uart_puts("Enabling SD clock output...\n");
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN;

    // Small delay to allow clock to stabilize
    for (volatile int i = 0; i < 10000; i++);

    // Step 5: Send CMD0 (reset)
    uart_puts("Sending CMD0 (reset)...\n");
    if (emmc_cmd(0x00000000, 0) != 0){
        uart_puts("ERROR: CMD0 failed\n");
        return -1;
    }
    uart_puts("CMD0 sent successfully\n");

    // Step 6: Send CMD8 (voltage check) - for SD v2.0+ cards
    uart_puts("Sending CMD8 (voltage check)...\n");
    if (emmc_cmd(0x08020000, 0x1AA) != 0){
        uart_puts("ERROR: CMD8 failed\n");
        return -1;
    }
    uart_puts("CMD8 sent successfully\n");

    // Step 7: ACMD41 loop - wait for card to be ready
    uart_puts("Initializing card with ACMD41...\n");
    for (int i = 0; i < 1000; i++){
        // CMD55 (App command)
        if (emmc_cmd(0x37000000, 0) != 0){
            uart_puts("ERROR: CMD55 failed\n");
            continue;
        }
        
        // ACMD41 (SD send operation conditions)
        if (emmc_cmd(0x29020000, 0x40FF8000) != 0){
            uart_puts("ERROR: ACMD41 failed\n");
            continue;
        }

        // Check if card is ready (bit 31 of response = card ready)
        if (*EMMC_RESP0 & (1 << 31)){
            uart_puts("Card ready!\n");
            return 0;
        }
        
        // Small delay between retries
        for (volatile int j = 0; j < 1000; j++);
    }

    uart_puts("ERROR: Card initialization failed - timeout on ACMD41\n");
    return -1;
}

int sd_read_block(unsigned int lba, unsigned char *buffer){
    // Set block size = 512, count = 1
    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    // CMD17 (read single block)
    if (emmc_cmd(0x11220010, lba) != 0){
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
