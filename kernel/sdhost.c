#include "uart.h"

#define SDHOST_BASE 0x3F202000

#define SDCMD   (*(volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SDARG   (*(volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SDTOUT  (*(volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SDCDIV  (*(volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SDRSP0  (*(volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SDHSTS  (*(volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SDVDD   (*(volatile unsigned int*)(SDHOST_BASE + 0x30))
#define SDHCFG  (*(volatile unsigned int*)(SDHOST_BASE + 0x38))

#define SDDATA (*(volatile unsigned int*)(SDHOST_BASE + 0x40))
#define SDHSTS_DATA_FLAG (1 << 0)
#define SDHSTS_ERROR_MASK 0x0000007E

#define SDHBCT (*(volatile unsigned int*)(SDHOST_BASE + 0x3C))
#define SDHBLC (*(volatile unsigned int*)(SDHOST_BASE + 0x50))

#define SDCMD_NEW_FLAG 0x8000
#define SDCMD_FAIL_FLAG 0x4000
#define SDCMD_BUSY_FLAG 0x2000

#define SDCMD_NO_RESPONSE 0x1000
#define SDCMD_LONG_RESPONSE 0x0800

#define SDCMD_READ_CMD 0x0400
#define SDCMD_WRITE_CMD 0x0200

#define CMD_NEEDS_RESP 1
#define CMD_LONG_RESP 2
#define CMD_IS_READ 4

static unsigned int sd_rca = 0;

static int sdhost_wait_resp(void){
    int timeout = 1000000;

    while((SDCMD & SDCMD_NEW_FLAG) && timeout--){
        // wait
    }
    if (!timeout){
        uart_puts("RESP TIMEOUT\n");
        return -1;
    }
    return 0;
}

unsigned int sdhost_get_resp(void){
    unsigned int r = SDRSP0;
    SDHSTS = 0x7F8;

    return r;
}

static void delay(int count) {
    while (count--) asm volatile("nop");
}

void sdhost_reset(void) {
    uart_puts("SDHOST RESET\n");

    SDVDD = 0;
    delay(10000);

    SDCMD = 0;
    SDARG = 0;
    SDTOUT = 0xF00000;
    SDCDIV = 0x00000148;
    SDHSTS = 0x7F8;
    SDHCFG = (1 << 0) | (1 << 1);

    delay(100000);

    SDVDD = 1;
    delay(500000);
}

int sdhost_cmd(unsigned int cmd, unsigned int arg, unsigned int flags) {
    int timeout;

    uart_puts("CMD ");
    uart_puthex(cmd);
    uart_puts("\n");

    // Wait until controller free
    timeout = 1000000;
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--);
    if (!timeout) {
        uart_puts("CMD BUSY TIMEOUT\n");
        return -1;
    }

    // Clear errors
    SDHSTS = 0x7F8;

    unsigned int sdcmd = cmd;

//    if (flags == 0) {
//        sdcmd |= SDCMD_NO_RESPONSE;
//    } else {
//        if (flags == 136)
//            sdcmd |= SDCMD_LONG_RESPONSE;
//    }

    if (!(flags & CMD_NEEDS_RESP)) sdcmd |= SDCMD_NO_RESPONSE;
    if (flags & CMD_LONG_RESP) sdcmd |= SDCMD_LONG_RESPONSE;
    if (flags & CMD_IS_READ) sdcmd |= SDCMD_READ_CMD;

    SDARG = arg;
    SDCMD = sdcmd | SDCMD_NEW_FLAG;

    timeout = 1000000;
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--);

    if (SDCMD & SDCMD_FAIL_FLAG){
        uart_puts("CMD FAIL\n");
    }

    // Wait for completion
//    if(sdhost_wait_resp() != 0){
//        return -1;
//    }

//    if (SDCMD & SDCMD_FAIL_FLAG) {
//        uart_puts("CMD FAIL\n");
//        return -1;
//    }

    uart_puts("CMD OK\n");
    return 0;
}

int sdhost_init_card(void) {
    uart_puts("SD INIT START\n");

    // CMD0 (reset)
    if (sdhost_cmd(0, 0, 0) != 0) return -1;

    delay(100000);
    
    int sd_v2 = 0;
    // CMD8 (check voltage)
    if (sdhost_cmd(8, 0x1AA, CMD_NEEDS_RESP) == 0) {
        unsigned int r = sdhost_get_resp();

        uart_puts("CMD8 OK RESP: ");
        uart_puthex(r);
        uart_puts("\n");

        if ((r & 0xFFF) == 0x1AA){
            sd_v2 = 1;
        }
    } else {
        uart_puts("CMD8 FAIL -> assuming SDv1\n");
    }

    unsigned int resp;

    int retries = 1000;

    do {
        if (sdhost_cmd(55, 0, CMD_NEEDS_RESP) != 0){
            uart_puts("CMD55 FAIL\n");
            return -1;
        }
        uart_puts("CMD55 RESP = ");
        uart_puthex(SDRSP0);
        uart_puts("\n");

        unsigned int acmd41_arg;

        if (sd_v2){
            acmd41_arg = 0x40300000;
        }
        else { acmd41_arg = 0x00300000; }

        if (sdhost_cmd(41, acmd41_arg, CMD_NEEDS_RESP) != 0){
            uart_puts("ACMD41 FAIL\n");
            return -1;
        }

        resp = sdhost_get_resp();

        uart_puts("ACMD41 RESP= ");
        uart_puthex(resp);
        uart_puts("\n");

        if (resp & 0x80000000) break;

        delay(10000);

    } while (-- retries);

    if (retries == 0){
        uart_puts("ACMD41 TIMEOUT\n");
        return -1;
    }

    uart_puts("CARD READY! \n");
    uart_puthex(resp);
    uart_puts("\n");

    if (sdhost_cmd(2, 0, CMD_NEEDS_RESP | CMD_LONG_RESP) != 0){
        uart_puts("CMD2 FAIL\n");
        return -1;
    }

    if (sdhost_cmd(3, 0, CMD_NEEDS_RESP) != 0){
        uart_puts("CMD3 FAIL\n");
        return -1;
    }

    sd_rca = sdhost_get_resp() >> 16;

    uart_puts("RCA = ");
    uart_puthex(sd_rca);
    uart_puts("\n");

    if (sdhost_cmd(7, sd_rca << 16, CMD_NEEDS_RESP) != 0){
        uart_puts("CMD7 FAIL\n");
        return -1;
    }

    uart_puts("CARD SELECTED\n");

    return 0;
}

int sdhost_read_block(unsigned int lba, unsigned char *buffer){
    uart_puts("READ BLOCK ");
    uart_puthex(lba);
    uart_puts("\n");

    SDHBCT = 512;
    SDHBLC = 1;

    if (sdhost_cmd(17, lba, CMD_NEEDS_RESP | CMD_IS_READ) != 0){
        uart_puts("CMD17 FAIL\n");
        return -1;
    }

    uart_puts("CMD17 OK, reading data... \n");


    delay(50000);
    for (int i = 0; i < 128; i++){
        int timeout = 1000000;
        while(!(SDHSTS & SDHSTS_DATA_FLAG) && timeout--);

        if (!timeout){
            uart_puts("DATA TIMEOUT\n");
            return -1;
        }
        
        unsigned int data = SDDATA;
        buffer[i*4+0] = (data >> 0) & 0xFF;
        buffer[i*4+1] = (data >> 8) & 0xFF;
        buffer[i*4+2] = (data >> 16) & 0xFF;
        buffer[i*4+3] = (data >> 24) & 0xFF;
    }
    SDHSTS = SDHSTS;
    uart_puts("READ DONE!\n");
    return 0;
}
