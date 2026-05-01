#include "emmc.h"
#include "mailbox.h"
#include "timer.h"
#include "uart.h"

#define EMMC_BASE 0x3F300000UL

#define EMMC_ARG2        (*(volatile unsigned int*)(EMMC_BASE + 0x00))
#define EMMC_BLKSIZECNT  (*(volatile unsigned int*)(EMMC_BASE + 0x04))
#define EMMC_ARG1        (*(volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM       (*(volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0       (*(volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_DATA        (*(volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS      (*(volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CONTROL0    (*(volatile unsigned int*)(EMMC_BASE + 0x28))
#define EMMC_CONTROL1    (*(volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTERRUPT   (*(volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_IRPT_MASK   (*(volatile unsigned int*)(EMMC_BASE + 0x34))
#define EMMC_IRPT_EN     (*(volatile unsigned int*)(EMMC_BASE + 0x38))

#define INT_CMD_DONE     (1u << 0)
#define INT_DATA_DONE    (1u << 1)
#define INT_READ_RDY     (1u << 5)
#define INT_ERR          (1u << 15)
#define INT_ERROR_MASK   0xFFFF0000u

#define SR_CMD_INHIBIT   (1u << 0)
#define SR_DAT_INHIBIT   (1u << 1)

#define C1_CLK_INTLEN    (1u << 0)
#define C1_CLK_STABLE    (1u << 1)
#define C1_CLK_EN        (1u << 2)
#define C1_SRST_HC       (1u << 24)

#define CMD_RSPNS_NONE   (0u << 16)
#define CMD_RSPNS_136    (1u << 16)
#define CMD_RSPNS_48     (2u << 16)
#define CMD_CRCCHK_EN    (1u << 19)
#define CMD_IXCHK_EN     (1u << 20)
#define CMD_ISDATA       (1u << 21)

#define TM_BLKCNT_EN     (1u << 1)
#define TM_DAT_DIR_CH    (1u << 4)

static unsigned int g_rca = 0;
static int g_sdhc = 0;

static void short_delay(unsigned int c){
    while (c--) asm volatile("nop");
}

static int wait_status_clear(unsigned int mask, unsigned long timeout_ms){
    unsigned long start = system_ticks;
    while (EMMC_STATUS & mask){
        if ((system_ticks - start) > timeout_ms){
            return -1;
        }
    }
    return 0;
}

static int wait_interrupt(unsigned int mask, unsigned long timeout_ms){
    unsigned long start = system_ticks;
    while (1){
        unsigned int irpt = EMMC_INTERRUPT;
        if (irpt & INT_ERROR_MASK){
            EMMC_INTERRUPT = irpt;
            return -1;
        }
        if (irpt & INT_ERR){
            EMMC_INTERRUPT = irpt;
            return -1;
        }
        if (irpt & mask){
            EMMC_INTERRUPT = irpt;
            return 0;
        }
        if ((system_ticks - start) > timeout_ms){
            return -1;
        }
    }
}

static int emmc_cmd(unsigned int cmd, unsigned int arg, unsigned int flags){
    if (wait_status_clear(SR_CMD_INHIBIT, 120) != 0){
        return -1;
    }

    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_ARG1 = arg;
    EMMC_CMDTM = (cmd << 24) | flags;

    if (wait_interrupt(INT_CMD_DONE, 120) != 0){
        return -1;
    }
    return 0;
}

static int emmc_acmd41(void){
    unsigned int resp = 0;
    for (int i = 0; i < 2000; i++){
        if (emmc_cmd(55, g_rca << 16, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN) != 0){
            return -1;
        }
        if (emmc_cmd(41, 0x40300000, CMD_RSPNS_48) != 0){
            return -1;
        }
        resp = EMMC_RESP0;
        if (resp & 0x80000000u){
            g_sdhc = (resp & (1u << 30)) ? 1 : 0;
            return 0;
        }
        short_delay(2000);
    }
    return -1;
}

int emmc_init(void){
    mailbox_set_emmc_clock(50000000);

    EMMC_CONTROL1 |= C1_SRST_HC;
    {
        unsigned long start = system_ticks;
        while (EMMC_CONTROL1 & C1_SRST_HC){
            if ((system_ticks - start) > 200){
                return -1;
            }
        }
    }

    EMMC_CONTROL0 = 0;
    EMMC_CONTROL1 &= ~(0xFFFu << 16);
    EMMC_CONTROL1 |= (250u << 16); // slow init clock
    EMMC_CONTROL1 |= C1_CLK_INTLEN;
    {
        unsigned long start = system_ticks;
        while (!(EMMC_CONTROL1 & C1_CLK_STABLE)){
            if ((system_ticks - start) > 200){
                return -1;
            }
        }
    }
    EMMC_CONTROL1 |= C1_CLK_EN;

    EMMC_INTERRUPT = 0xFFFFFFFFu;
    EMMC_IRPT_MASK = 0xFFFFFFFFu;
    EMMC_IRPT_EN = 0xFFFFFFFFu;

    g_rca = 0;
    g_sdhc = 0;

    if (emmc_cmd(0, 0, CMD_RSPNS_NONE) != 0) return -1;
    short_delay(5000);
    (void)emmc_cmd(8, 0x1AA, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN);
    if (emmc_acmd41() != 0) return -1;
    if (emmc_cmd(2, 0, CMD_RSPNS_136 | CMD_CRCCHK_EN) != 0) return -1;
    if (emmc_cmd(3, 0, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN) != 0) return -1;

    g_rca = (EMMC_RESP0 >> 16) & 0xFFFFu;
    if (g_rca == 0) return -1;

    if (emmc_cmd(7, g_rca << 16, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN) != 0) return -1;
    if (emmc_cmd(16, 512, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN) != 0) return -1;

    uart_puts("EMMC init OK\n");
    return 0;
}

int emmc_read_block(unsigned int lba, unsigned char *buffer){
    unsigned int addr = g_sdhc ? lba : (lba * 512u);

    if (wait_status_clear(SR_CMD_INHIBIT | SR_DAT_INHIBIT, 120) != 0){
        return -1;
    }

    EMMC_BLKSIZECNT = (1u << 16) | 512u;
    EMMC_INTERRUPT = 0xFFFFFFFFu;

    if (emmc_cmd(17, addr, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA | TM_DAT_DIR_CH | TM_BLKCNT_EN) != 0){
        return -1;
    }

    if (wait_interrupt(INT_READ_RDY, 120) != 0){
        return -1;
    }

    for (int i = 0; i < 128; i++){
        unsigned int d = EMMC_DATA;
        buffer[i*4 + 0] = (unsigned char)(d & 0xFF);
        buffer[i*4 + 1] = (unsigned char)((d >> 8) & 0xFF);
        buffer[i*4 + 2] = (unsigned char)((d >> 16) & 0xFF);
        buffer[i*4 + 3] = (unsigned char)((d >> 24) & 0xFF);
    }

    if (wait_interrupt(INT_DATA_DONE, 120) != 0){
        return -1;
    }

    return 0;
}
