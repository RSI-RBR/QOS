#include "sd.h"
#include "uart.h"

#define EMMC_BASE 0x3F300000

#define EMMC_ARG1     ((volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM    ((volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0    ((volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_RESP1    ((volatile unsigned int*)(EMMC_BASE + 0x14))
#define EMMC_RESP2    ((volatile unsigned int*)(EMMC_BASE + 0x18))
#define EMMC_RESP3    ((volatile unsigned int*)(EMMC_BASE + 0x1C))
#define EMMC_DATA     ((volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS   ((volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CONTROL1 ((volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT ((volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_BLKSIZECNT ((volatile unsigned int*)(EMMC_BASE + 0x04))
#define EMMC_IRPT_EN  ((volatile unsigned int*)(EMMC_BASE + 0x38))

#define EMMC_CONTROL1_CLK_INTLEN  (1 << 0)
#define EMMC_CONTROL1_CLK_STABLE  (1 << 1)
#define EMMC_CONTROL1_CLK_EN      (1 << 2)
#define EMMC_CONTROL1_RESET_HC    (1 << 24)

#define EMMC_INT_CMD_DONE    (1 << 0)
#define EMMC_INT_DATA_DONE   (1 << 1)
#define EMMC_INT_CMD_TIMEOUT (1 << 16)

#define EMMC_CMD_RESP_NONE (0 << 16)
#define EMMC_CMD_RESP_48   (2 << 16)
#define EMMC_CMD_RESP_136  (1 << 16)
#define EMMC_CMD_DATA      (1 << 21)

#define GPIO_BASE 0x3F200000

#define GPFSEL4 ((volatile unsigned int*)(GPIO_BASE + 0x10))
#define GPFSEL5 ((volatile unsigned int*)(GPIO_BASE + 0x14))
#define GPPUD   ((volatile unsigned int*)(GPIO_BASE + 0x94))
#define GPPUDCLK1 ((volatile unsigned int*)(GPIO_BASE + 0x9C))

static void delay_cycles(int count) {
    while(count--) asm volatile("nop");
}

void sd_gpio_init(void){
    uart_puts("Configuring SD GPIO...\n");

    unsigned int val;

    // GPIO48,49 (GPFSEL4)
    val = *GPFSEL4;

    val &= ~(7 << 24); // GPIO48
    val |=  (7 << 24);

    val &= ~(7 << 27); // GPIO49
    val |=  (7 << 27);

    *GPFSEL4 = val;

    // GPIO50–53 (GPFSEL5)
    val = *GPFSEL5;

    val &= ~(7 << 0);  // GPIO50
    val |=  (7 << 0);

    val &= ~(7 << 3);  // GPIO51
    val |=  (7 << 3);

    val &= ~(7 << 6);  // GPIO52
    val |=  (7 << 6);

    val &= ~(7 << 9);  // GPIO53
    val |=  (7 << 9);

    *GPFSEL5 = val;

    // Disable pull-up/down
    *GPPUD = 0;
    delay_cycles(150);

    *GPPUDCLK1 = (1 << (48-32)) | (1 << (49-32)) |
                 (1 << (50-32)) | (1 << (51-32)) |
                 (1 << (52-32)) | (1 << (53-32));

    delay_cycles(150);

    *GPPUDCLK1 = 0;

    uart_puts("GPIO configured\n");
}

static unsigned int rca = 0;
static int is_sdhc = 0;

static void delay(int x) {
    for (volatile int i = 0; i < x * 1000; i++);
}

static int wait_cmd_done() {
    int timeout = 1000000;
    while (timeout--) {
        unsigned int s = *EMMC_INTERRUPT;

        if (s & EMMC_INT_CMD_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_CMD_DONE;
            return 0;
        }

        if (s & EMMC_INT_CMD_TIMEOUT) {
            uart_puts("CMD TIMEOUT\n");
            return -1;
        }
    }
    uart_puts("CMD WAIT TIMEOUT\n");
    return -1;
}

static int wait_data_done() {
    int timeout = 1000000;
    while (timeout--) {
        if (*EMMC_INTERRUPT & EMMC_INT_DATA_DONE) {
            *EMMC_INTERRUPT = EMMC_INT_DATA_DONE;
            return 0;
        }
    }
    uart_puts("DATA TIMEOUT\n");
    return -1;
}

static int cmd(unsigned int idx, unsigned int arg, unsigned int resp) {
    while (*EMMC_STATUS & 1); // wait command line free

    *EMMC_INTERRUPT = 0xFFFFFFFF;

    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = (idx << 24) | resp;

    if (idx == 0) {
        delay(10);
        return 0;
    }

    return wait_cmd_done();
}

int sd_init(void) {
    uart_puts("SD init start\n");

    // Reset controller
    *EMMC_CONTROL1 |= EMMC_CONTROL1_RESET_HC;
    int timeout = 100000;
    while ((*EMMC_CONTROL1 & EMMC_CONTROL1_RESET_HC) && timeout--);
    if (timeout <= 0) {
        uart_puts("Reset fail\n");
        return -1;
    }

    *EMMC_INTERRUPT = 0xFFFFFFFF;
    *EMMC_IRPT_EN = 0xFFFFFFFF;

    // Clock setup
    unsigned int c = *EMMC_CONTROL1 & ~0xFFF;
    *EMMC_CONTROL1 = c | 240;
    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_INTLEN;

    timeout = 100000;
    while (!(*EMMC_CONTROL1 & EMMC_CONTROL1_CLK_STABLE) && timeout--);
    if (timeout <= 0) {
        uart_puts("Clock fail\n");
        return -1;
    }

    *EMMC_CONTROL1 |= EMMC_CONTROL1_CLK_EN;
    delay(100);

    // CMD0
    uart_puts("CMD0\n");
    if (cmd(0, 0, EMMC_CMD_RESP_NONE)) return -1;

    // CMD8
    uart_puts("CMD8\n");
    if (cmd(8, 0x1AA, EMMC_CMD_RESP_48)) return -1;

    unsigned int resp = *EMMC_RESP0;
    uart_puts("CMD8 resp: ");
    uart_puthex(resp);
    uart_puts("\n");

    // ACMD41 loop
    uart_puts("ACMD41\n");
    for (int i = 0; i < 1000; i++) {
        cmd(55, 0, EMMC_CMD_RESP_48);
        cmd(41, 0x40300000, EMMC_CMD_RESP_48);

        resp = *EMMC_RESP0;

        if (resp & (1 << 31)) {
            uart_puts("Card ready\n");

            if (resp & (1 << 30)) {
                is_sdhc = 1;
                uart_puts("SDHC detected\n");
            }

            break;
        }

        delay(10);
    }

    // CMD2
    uart_puts("CMD2\n");
    if (cmd(2, 0, EMMC_CMD_RESP_136)) return -1;

    // CMD3
    uart_puts("CMD3\n");
    if (cmd(3, 0, EMMC_CMD_RESP_48)) return -1;

    rca = *EMMC_RESP0 & 0xFFFF0000;

    uart_puts("RCA: ");
    uart_puthex(rca);
    uart_puts("\n");

    // CMD7 (select)
    uart_puts("CMD7\n");
    if (cmd(7, rca, EMMC_CMD_RESP_48)) return -1;

    // CMD16 (set block size = 512)
    uart_puts("CMD16\n");
    if (cmd(16, 512, EMMC_CMD_RESP_48)) return -1;

    uart_puts("SD init done\n");

    return 0;
}

int sd_read_block(unsigned int lba, unsigned char *buffer) {
    unsigned int addr = is_sdhc ? lba : lba * 512;

    *EMMC_BLKSIZECNT = (1 << 16) | 512;

    // CMD17
    if (cmd(17, addr, EMMC_CMD_RESP_48 | EMMC_CMD_DATA)) {
        uart_puts("CMD17 fail\n");
        return -1;
    }

    if (wait_data_done()) {
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
