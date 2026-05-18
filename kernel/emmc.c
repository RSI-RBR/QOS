#include "emmc.h"
#include "mailbox.h"
#include "platform/mmio.h"
#include "timer.h"
#include "uart.h"

#define EMMC_BASE QOS_EMMC_BASE

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

#define INT_CMD_DONE      (1u << 0)
#define INT_DATA_DONE     (1u << 1)
#define INT_WRITE_RDY     (1u << 4)
#define INT_READ_RDY      (1u << 5)
#define INT_ERR           (1u << 15)
#define INT_ERROR_MASK    0xFFFF0000u
#define INT_DATA_TIMEOUT  (1u << 20)

#define SR_CMD_INHIBIT    (1u << 0)
#define SR_DAT_INHIBIT    (1u << 1)

#define C1_CLK_INTLEN       (1u << 0)
#define C1_CLK_STABLE       (1u << 1)
#define C1_CLK_EN           (1u << 2)
#define C1_SRST_HC          (1u << 24)
#define C1_SRST_CMD         (1u << 25)
#define C1_SRST_DAT         (1u << 26)
#define C1_CLK_DIV_MASK     ((0xFFu << 8) | (0x3u << 6))
#define C1_DATA_TOUNIT_MASK (0xFu << 16)
#define C1_DATA_TOUNIT_MAX  (0xEu << 16)

#define C0_HCTL_DWIDTH    (1u << 1)
#define C0_SD_BUS_POWER   (1u << 8)
#define C0_SD_BUS_VOLT_33 (7u << 9)

#define CMD_RSPNS_NONE    (0u << 16)
#define CMD_RSPNS_136     (1u << 16)
#define CMD_RSPNS_48      (2u << 16)
#define CMD_RSPNS_48B     (3u << 16)
#define CMD_RSPNS_MASK    (3u << 16)
#define CMD_CRCCHK_EN     (1u << 19)
#define CMD_IXCHK_EN      (1u << 20)
#define CMD_ISDATA        (1u << 21)
#define CMD_TYPE_ABORT    (3u << 22)

#define TM_BLKCNT_EN      (1u << 1)
#define TM_AUTO_CMD12     (1u << 2)
#define TM_DAT_DIR_CH     (1u << 4)
#define TM_MULTI_BLOCK    (1u << 5)

static unsigned int g_rca = 0;
static int g_sdhc = 0;

static void short_delay(unsigned int c);

static int emmc_set_clock_divider(unsigned int div){
    unsigned int c1;

    if (div > 0x3FFu){
        return -1;
    }

    c1 = EMMC_CONTROL1;
    c1 &= ~C1_CLK_EN;
    EMMC_CONTROL1 = c1;
    short_delay(1000);

    c1 &= ~(C1_CLK_DIV_MASK | C1_DATA_TOUNIT_MASK);
    c1 |= C1_CLK_INTLEN | C1_DATA_TOUNIT_MAX;
    c1 |= ((div & 0xFFu) << 8);
    c1 |= (((div >> 8) & 0x3u) << 6);
    EMMC_CONTROL1 = c1;

    {
        unsigned long start = system_ticks;
        unsigned int spin = 20000000;
        while (!(EMMC_CONTROL1 & C1_CLK_STABLE)){
            if ((system_ticks - start) > 200 || --spin == 0){
                return -1;
            }
        }
    }

    EMMC_CONTROL1 |= C1_CLK_EN;
    short_delay(1000);
    return 0;
}

static void short_delay(unsigned int c){
    while (c--) asm volatile("nop");
}

static void emmc_clear_interrupts(void){
    EMMC_INTERRUPT = 0xFFFFFFFFu;
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

static int wait_irq(unsigned int mask, unsigned long timeout_ms, unsigned int *irpt_out){
    unsigned long start = system_ticks;
    while (1){
        unsigned int irpt = EMMC_INTERRUPT;
        if (irpt & (mask | INT_ERROR_MASK | INT_ERR)){
            if (irpt_out) *irpt_out = irpt;
            return 0;
        }
        if ((system_ticks - start) > timeout_ms){
            if (irpt_out) *irpt_out = EMMC_INTERRUPT;
            return -1;
        }
    }
}

static int emmc_reset_cmd_line(void){
    EMMC_CONTROL1 |= C1_SRST_CMD;
    {
        unsigned long start = system_ticks;
        while (EMMC_CONTROL1 & C1_SRST_CMD){
            if ((system_ticks - start) > 120){
                return -1;
            }
        }
    }
    return 0;
}

static int emmc_reset_dat_line(void){
    EMMC_CONTROL1 |= C1_SRST_DAT;
    {
        unsigned long start = system_ticks;
        while (EMMC_CONTROL1 & C1_SRST_DAT){
            if ((system_ticks - start) > 120){
                return -1;
            }
        }
    }
    return 0;
}

static int emmc_cmd(unsigned int cmd, unsigned int arg, unsigned int flags, unsigned long timeout_ms){
    unsigned int irpt = 0;

    if (wait_status_clear(SR_CMD_INHIBIT, 120) != 0){
        return -1;
    }

    if ((flags & CMD_ISDATA) ||
        (((flags & CMD_RSPNS_MASK) == CMD_RSPNS_48B) && cmd != 12)){
        if (wait_status_clear(SR_DAT_INHIBIT, 120) != 0){
            return -1;
        }
    }

    emmc_clear_interrupts();
    EMMC_ARG1 = arg;
    EMMC_CMDTM = (cmd << 24) | flags;

    if (wait_irq(INT_CMD_DONE, timeout_ms, &irpt) != 0){
        return -1;
    }

    EMMC_INTERRUPT = (irpt & (INT_CMD_DONE | INT_ERROR_MASK | INT_ERR));

    if (irpt & (INT_ERROR_MASK | INT_ERR)){
        return -1;
    }

    return 0;
}

static int emmc_cmd_app(unsigned int acmd, unsigned int arg, unsigned int flags, unsigned long timeout_ms){
    if (emmc_cmd(55, g_rca << 16, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, timeout_ms) != 0){
        return -1;
    }
    return emmc_cmd(acmd, arg, flags, timeout_ms);
}

static int emmc_get_state(unsigned int *state){
    if (g_rca == 0){
        return -1;
    }
    if (emmc_cmd(13, g_rca << 16, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
        return -1;
    }
    *state = (EMMC_RESP0 >> 9) & 0xFu;
    return 0;
}

static int emmc_select_card(void){
    return emmc_cmd(7, g_rca << 16, CMD_RSPNS_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120);
}

static int emmc_stop_transmission(void){
    if (emmc_cmd(12, 0, CMD_RSPNS_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_TYPE_ABORT, 120) != 0){
        return -1;
    }
    return emmc_reset_dat_line();
}

static int emmc_ensure_data_mode(void){
    unsigned int state = 0;

    if (emmc_get_state(&state) != 0){
        return -1;
    }

    if (state == 3){
        if (emmc_select_card() != 0){
            return -1;
        }
    } else if (state == 5){
        if (emmc_stop_transmission() != 0){
            return -1;
        }
    } else if (state != 4){
        return -1;
    }

    if (emmc_get_state(&state) != 0){
        return -1;
    }

    return (state == 4) ? 0 : -1;
}

static int emmc_acmd41(void){
    unsigned int resp = 0;
    for (int i = 0; i < 2000; i++){
        if (emmc_cmd_app(41, 0x40300000, CMD_RSPNS_48, 120) != 0){
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
    uart_puts("EMMC: init start\n");
    mailbox_set_emmc_clock(50000000);

    EMMC_CONTROL1 |= C1_SRST_HC;
    {
        unsigned long start = system_ticks;
        unsigned int spin = 20000000;
        while (EMMC_CONTROL1 & C1_SRST_HC){
            if ((system_ticks - start) > 200 || --spin == 0){
                uart_puts("EMMC: reset clear timeout\n");
                return -1;
            }
        }
    }

    EMMC_CONTROL0 = C0_SD_BUS_VOLT_33 | C0_SD_BUS_POWER;
    short_delay(10000);

    if (emmc_set_clock_divider(128u) != 0){
        uart_puts("EMMC: clk stable timeout\n");
        return -1;
    }

    if (emmc_reset_cmd_line() != 0 || emmc_reset_dat_line() != 0){
        uart_puts("EMMC: line reset fail\n");
        return -1;
    }

    emmc_clear_interrupts();
    EMMC_IRPT_MASK = 0xFFFFFFFFu;
    EMMC_IRPT_EN = 0xFFFFFFFFu;

    g_rca = 0;
    g_sdhc = 0;

    if (emmc_cmd(0, 0, CMD_RSPNS_NONE, 120) != 0){
        uart_puts("EMMC: CMD0 fail\n");
        return -1;
    }

    short_delay(5000);
    (void)emmc_cmd(8, 0x1AA, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120);

    if (emmc_acmd41() != 0){
        uart_puts("EMMC: ACMD41 fail\n");
        return -1;
    }

    if (emmc_cmd(2, 0, CMD_RSPNS_136 | CMD_CRCCHK_EN, 120) != 0){
        uart_puts("EMMC: CMD2 fail\n");
        return -1;
    }

    if (emmc_cmd(3, 0, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
        uart_puts("EMMC: CMD3 fail\n");
        return -1;
    }

    g_rca = (EMMC_RESP0 >> 16) & 0xFFFFu;
    if (g_rca == 0){
        uart_puts("EMMC: invalid RCA\n");
        return -1;
    }

    if (emmc_select_card() != 0){
        uart_puts("EMMC: CMD7 fail\n");
        return -1;
    }

    if (!g_sdhc){
        if (emmc_cmd(16, 512, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
            uart_puts("EMMC: CMD16 fail\n");
            return -1;
        }
    }

    if (emmc_cmd_app(6, 2, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) == 0){
        EMMC_CONTROL0 |= C0_HCTL_DWIDTH;
    }

    /*
     * Identification is intentionally slow, but keeping that divider for
     * normal file IO makes asset loads crawl. With the mailbox clock at
     * 50 MHz, divider 4 is the stable fast-transfer clock for Pi 3 SD
     * cards while still being far faster than ID mode.
     */
    if (emmc_set_clock_divider(4u) != 0){
        uart_puts("EMMC: fast clock failed\n");
        return -1;
    }

    uart_puts("EMMC init OK\n");
    return 0;
}

int emmc_read_block(unsigned int lba, unsigned char *buffer){
    unsigned int addr;
    unsigned int irpt;

    if (!buffer){
        return -1;
    }

    for (int attempt = 0; attempt < 3; attempt++){
        if (emmc_ensure_data_mode() != 0){
            emmc_reset_cmd_line();
            emmc_reset_dat_line();
            continue;
        }

        addr = g_sdhc ? lba : (lba * 512u);

        EMMC_BLKSIZECNT = (1u << 16) | 512u;

        if (emmc_cmd(17, addr,
                     CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
                     CMD_ISDATA | TM_DAT_DIR_CH | TM_BLKCNT_EN,
                     200) != 0){
            emmc_stop_transmission();
            continue;
        }

        if (wait_irq(INT_READ_RDY, 200, &irpt) != 0){
            emmc_stop_transmission();
            continue;
        }
        EMMC_INTERRUPT = (irpt & (INT_READ_RDY | INT_ERROR_MASK | INT_ERR));
        if (irpt & (INT_ERROR_MASK | INT_ERR)){
            emmc_stop_transmission();
            continue;
        }

        for (int i = 0; i < 128; i++){
            unsigned int d = EMMC_DATA;
            buffer[i * 4 + 0] = (unsigned char)(d & 0xFFu);
            buffer[i * 4 + 1] = (unsigned char)((d >> 8) & 0xFFu);
            buffer[i * 4 + 2] = (unsigned char)((d >> 16) & 0xFFu);
            buffer[i * 4 + 3] = (unsigned char)((d >> 24) & 0xFFu);
        }

        if (wait_irq(INT_DATA_DONE, 200, &irpt) != 0){
            emmc_stop_transmission();
            continue;
        }
        EMMC_INTERRUPT = (irpt & (INT_DATA_DONE | INT_ERROR_MASK | INT_ERR));

        if ((irpt & (INT_ERROR_MASK | INT_ERR)) &&
            ((irpt & (INT_DATA_DONE | INT_DATA_TIMEOUT)) != (INT_DATA_DONE | INT_DATA_TIMEOUT))){
            emmc_stop_transmission();
            continue;
        }

        if (wait_status_clear(SR_DAT_INHIBIT, 200) != 0){
            emmc_stop_transmission();
            continue;
        }

        return 0;
    }

    return -1;
}

int emmc_read_blocks(unsigned int lba, unsigned int count, unsigned char *buffer){
    unsigned int addr;
    unsigned int irpt;

    if (!buffer || count == 0u){
        return -1;
    }
    if (count == 1u){
        return emmc_read_block(lba, buffer);
    }
    if (count > 0xFFFFu){
        return -1;
    }

    for (int attempt = 0; attempt < 3; attempt++){
        if (emmc_ensure_data_mode() != 0){
            emmc_reset_cmd_line();
            emmc_reset_dat_line();
            continue;
        }

        addr = g_sdhc ? lba : (lba * 512u);
        EMMC_BLKSIZECNT = (count << 16) | 512u;

        if (emmc_cmd(18, addr,
                     CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
                     CMD_ISDATA | TM_DAT_DIR_CH | TM_BLKCNT_EN |
                     TM_MULTI_BLOCK | TM_AUTO_CMD12,
                     500) != 0){
            emmc_stop_transmission();
            continue;
        }

        for (unsigned int block = 0u; block < count; block++){
            if (wait_irq(INT_READ_RDY, 500, &irpt) != 0){
                emmc_stop_transmission();
                goto retry;
            }
            EMMC_INTERRUPT = (irpt & (INT_READ_RDY | INT_ERROR_MASK | INT_ERR));
            if (irpt & (INT_ERROR_MASK | INT_ERR)){
                emmc_stop_transmission();
                goto retry;
            }

            unsigned char* out = buffer + (block * 512u);
            for (int i = 0; i < 128; i++){
                unsigned int d = EMMC_DATA;
                out[i * 4 + 0] = (unsigned char)(d & 0xFFu);
                out[i * 4 + 1] = (unsigned char)((d >> 8) & 0xFFu);
                out[i * 4 + 2] = (unsigned char)((d >> 16) & 0xFFu);
                out[i * 4 + 3] = (unsigned char)((d >> 24) & 0xFFu);
            }
        }

        if (wait_irq(INT_DATA_DONE, 500, &irpt) != 0){
            emmc_stop_transmission();
            continue;
        }
        EMMC_INTERRUPT = (irpt & (INT_DATA_DONE | INT_ERROR_MASK | INT_ERR));
        if (irpt & (INT_ERROR_MASK | INT_ERR)){
            emmc_stop_transmission();
            continue;
        }
        if (wait_status_clear(SR_DAT_INHIBIT, 500) != 0){
            emmc_stop_transmission();
            continue;
        }

        return 0;

retry:
        continue;
    }

    return -1;
}

int emmc_write_block(unsigned int lba, const unsigned char *buffer){
    unsigned int addr;
    unsigned int irpt;

    if (!buffer){
        return -1;
    }

    for (int attempt = 0; attempt < 3; attempt++){
        if (emmc_ensure_data_mode() != 0){
            emmc_reset_cmd_line();
            emmc_reset_dat_line();
            continue;
        }

        addr = g_sdhc ? lba : (lba * 512u);
        EMMC_BLKSIZECNT = (1u << 16) | 512u;

        if (emmc_cmd(24, addr,
                     CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
                     CMD_ISDATA | TM_BLKCNT_EN,
                     500) != 0){
            emmc_stop_transmission();
            continue;
        }

        if (wait_irq(INT_WRITE_RDY, 500, &irpt) != 0){
            emmc_stop_transmission();
            continue;
        }
        EMMC_INTERRUPT = (irpt & (INT_WRITE_RDY | INT_ERROR_MASK | INT_ERR));
        if (irpt & (INT_ERROR_MASK | INT_ERR)){
            emmc_stop_transmission();
            continue;
        }

        for (int i = 0; i < 128; i++){
            unsigned int j = (unsigned int)i * 4u;
            unsigned int d = ((unsigned int)buffer[j + 0u]) |
                             ((unsigned int)buffer[j + 1u] << 8) |
                             ((unsigned int)buffer[j + 2u] << 16) |
                             ((unsigned int)buffer[j + 3u] << 24);
            EMMC_DATA = d;
        }

        if (wait_irq(INT_DATA_DONE, 500, &irpt) != 0){
            emmc_stop_transmission();
            continue;
        }
        EMMC_INTERRUPT = (irpt & (INT_DATA_DONE | INT_ERROR_MASK | INT_ERR));
        if (irpt & (INT_ERROR_MASK | INT_ERR)){
            emmc_stop_transmission();
            continue;
        }
        if (wait_status_clear(SR_DAT_INHIBIT, 500) != 0){
            emmc_stop_transmission();
            continue;
        }

        return 0;
    }

    return -1;
}
