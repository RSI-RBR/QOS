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

static int wait_cmd_done(){
    while (!(*EMMC_INTERRUPT & 0x1));
    *EMMC_INTERRUPT = 0x1;
    return 0;
}

static int wait_data_done(){
    while (!(*EMMC_INTERRUPT & 0x2));
    *EMMC_INTERRUPT = 0x2;
    return 0;
}

static int emmc_cmd(unsigned int cmd, unsigned int arg){
    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = cmd;

    return wait_cmd_done();
}

int sd_init(void){
    uart_puts("Initializing EMMC...\n");

    // Reset controller
    *EMMC_CONTROL1 |= (1 << 24);

    while (*EMMC_CONTROL1 & (1 << 24));

    // Enable clock (very simplified)
    *EMMC_CONTROL1 |= (1 << 2); // enable internal clock

    while (!(*EMMC_CONTROL1 & (1 << 1)));

    // Send CMD0 (reset)
    emmc_cmd(0x00000000, 0);
    uart_puts("CMD0 sent\n");

    // CMD8 (voltage check)
    emmc_cmd(0x08020000, 0x1AA);
    uart_puts("CMD8 sent\n");

    // ACMD41 loop
    for (int i = 0; i < 1000; i++){
        emmc_cmd(0x37000000, 0); // CMD55
        emmc_cmd(0x29020000, 0x40FF8000); // ACMD41

        if (*EMMC_RESP0 & (1 << 31)){
            uart_puts("Card ready\n");
            return 0;
        }
    }

    uart_puts("Card init failed\n");
    return -1;
}

int sd_read_block(unsigned int lba, unsigned char *buffer){
    // set block size = 512, count = 1
    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    // CMD17 (read single block)
    emmc_cmd(0x11220010, lba);

    wait_data_done();

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
        if (sd_read_block(lba + i, buffer + (i * 512)) !=0) return -1;
    }

    return 0;
}

