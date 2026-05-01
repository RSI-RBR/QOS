#include "sdhost.h"

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

#define SDCMD_READ_CMD 0x40
#define CMD_IS_READ 4

#define SDCMD_NO_RESPONSE 0x400
#define SDCMD_LONG_RESPONSE 0x200
#define CMD_NEEDS_RESP 1
#define CMD_LONG_RESP 2

#define SDEDM (*(volatile unsigned int*)(SDHOST_BASE + 0x34))
#define SDEDM_READ_THRESHOLD_SHIFT 14
#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_THRESHOLD_MASK 0x1F
#define SDEDM_FSM_MASK 0xf
#define SDEDM_FSM_READDATA 0x2
#define SDEDM_FSM_READWAIT 0x4
#define SDEDM_FSM_READCRC 0x5

static unsigned int sd_rca = 0;
static int sd_is_sdhc = 0;
static void sdhost_drain_fifo(void){
    int timeout = 100000;
    while (timeout-- > 0){
        unsigned int status = SDHSTS;
        if (!(status & SDHSTS_DATA_FLAG)){
            break;
        }
        (void)SDDATA;
        barrier();
    }
}


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
    SDCDIV = 0x000007FF;
    SDHSTS = 0x7F8;

    unsigned int temp = SDEDM;
    temp &= ~((SDEDM_THRESHOLD_MASK << SDEDM_READ_THRESHOLD_SHIFT) | (SDEDM_THRESHOLD_MASK << SDEDM_WRITE_THRESHOLD_SHIFT));
    // Use threshold of 4 as per official Circle library
    temp |= (4 << SDEDM_READ_THRESHOLD_SHIFT);
    temp |= (4 << SDEDM_WRITE_THRESHOLD_SHIFT);

    SDEDM = temp;
    SDHCFG = (1 << 0) | (1 << 1) | (1 << 3);

    delay(100000);

    SDVDD = 1;
    delay(500000);
}

int sdhost_cmd(unsigned int cmd, unsigned int arg, unsigned int flags) {
    int timeout;

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

    if (!(flags & CMD_NEEDS_RESP)){
        sdcmd |= SDCMD_NO_RESPONSE;
    } else {
        if (flags & CMD_LONG_RESP){
            sdcmd |= SDCMD_LONG_RESPONSE;
        }
    }
    if (flags & CMD_IS_READ){ sdcmd |= SDCMD_READ_CMD; }

    SDARG = arg;
    SDCMD = sdcmd | SDCMD_NEW_FLAG;

    // Wait for completion
//    if(sdhost_wait_resp() != 0){
//        return -1;
//    }
    timeout = 1000000;
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--);
    if (!timeout) {
        uart_puts("CMD BUSY TIMEOUT\n");
        return -1;
    }

    if (SDCMD & SDCMD_FAIL_FLAG) {
        uart_puts("CMD FAIL\n");
        return -1;
    }

    // Small deterministic settle delay to avoid relying on UART print timing.
    delay(200);

    return 0;
}

void sdhost_check_status(void){
    uart_puts("CMD13 (SEND STATUS)\n");

    if (sdhost_cmd(13, sd_rca << 16, CMD_NEEDS_RESP) != 0){
        uart_puts("CMD13 FAIL\n");
        return;
    }

    unsigned int r = sdhost_get_resp();

    uart_puts("Status = ");
    uart_puthex(r);
    uart_puts("\n");

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

    if (resp & (1 << 30)){
        sd_is_sdhc = 1;
        uart_puts("SDHC card detected\n");
    } else{
        sd_is_sdhc = 0;
        uart_puts("SDSC card detected\n");
    }

    uart_puts("CARD READY! \n");
    uart_puthex(resp);
    uart_puts("\n");


    if (sdhost_cmd(2, 0, CMD_NEEDS_RESP | CMD_LONG_RESP) != 0){
        uart_puts("CMD2 FAIL\n");
        return -1;
    }

    uart_puts("CMD2 RESP = ");
    uart_puthex(sdhost_get_resp());
    uart_puts("\n");

    if (sdhost_cmd(3, 0, CMD_NEEDS_RESP) != 0){
        uart_puts("CMD3 FAIL\n");
        return -1;
    }

    uart_puts("CMD3 RESP = ");
    uart_puthex(sdhost_get_resp());
    uart_puts("\n");

    sd_rca = sdhost_get_resp() >> 16;
    sd_rca &= 0xFFFF;

    uart_puts("RCA = ");
    uart_puthex(sd_rca);
    uart_puts("\n");

    if (sdhost_cmd(7, sd_rca << 16, CMD_NEEDS_RESP) != 0){
        uart_puts("CMD7 FAIL\n");
        return -1;
    }
    
    unsigned int r = sdhost_get_resp();
    uart_puts("CMD7 RESP = ");
    uart_puthex(r);
    uart_puts("\n");
    delay(100000);
    sdhost_check_status();

//    if ((r & 0xF00) != 0x900){
//        uart_puts("NOT IN TRANSFER STATE!\n");
//
//        if (sdhost_cmd(7, sd_rca << 16, CMD_NEEDS_RESP) != 0){
//            uart_puts("CMD7 RETRY FAIL\n");
//            return -1;
//        }
//
//        r = sdhost_get_resp();
//        uart_puts("CMD7 RESP = ");
//        uart_puthex(r);
//        uart_puts("\n");
//        
////        return -1
//    }
    uart_puts("CARD SELECTED\n");

    return 0;
}

int sdhost_read_block(unsigned int lba, unsigned char *buffer){
    int rc = -1;
    int irq_masked = 0;

    sdhost_drain_fifo();

    SDHBCT = 512;
    SDHBLC = 1;

    unsigned int addr = lba;
    if (!sd_is_sdhc){
        addr = lba * 512;
    }

    SDHSTS = 0x7F8;

    if (sdhost_cmd(17, addr, CMD_NEEDS_RESP | CMD_IS_READ) != 0){
        uart_puts("CMD17 FAIL\n");
        return -1;
    }
    (void)sdhost_get_resp();

    asm volatile("msr daifset, #2");
    irq_masked = 1;

    int words_left = 128;
    int index = 0;

    while (words_left > 0){
        unsigned int edm = SDEDM;
        unsigned int fifo_words = (edm >> 4) & 0x1F;
        unsigned int fsm = edm & SDEDM_FSM_MASK;
        barrier();

        if (fsm == 0 || fifo_words == 0){
            continue;
        }

        int burst = fifo_words;
        if (burst > words_left){
            burst = words_left;
        }

        for (int j = 0; j < burst; j++){
            if (index >= 512){
                uart_puts("BUFFER OVERFLOW\n");
                goto out;
            }

            unsigned int data = SDDATA;
            barrier();
            buffer[index++] = (data >> 0) & 0xFF;
            buffer[index++] = (data >> 8) & 0xFF;
            buffer[index++] = (data >> 16) & 0xFF;
            buffer[index++] = (data >> 24) & 0xFF;
            words_left--;

            if (SDHSTS & SDHSTS_ERROR_MASK){
                uart_puts("DATA ERROR\n");
                SDHSTS = 0x7F8;
                goto out;
            }
        }
    }

    if (SDHSTS & SDHSTS_ERROR_MASK){
        uart_puts("FINAL DATA ERROR status=");
        uart_puthex(SDHSTS);
        uart_puts("\n");
        SDHSTS = 0x7F8;
        goto out;
    }

    SDHSTS = 0x7F8;
    rc = 0;

    {
        int settle_timeout = 100000;
        while (settle_timeout--){
            if ((SDEDM & SDEDM_FSM_MASK) == 0){
                break;
            }
        }
    }

out:
    if (irq_masked){
        asm volatile("msr daifclr, #2");
    }
    sdhost_drain_fifo();
    return rc;
}




