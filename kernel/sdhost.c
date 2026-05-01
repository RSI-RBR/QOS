#include "sdhost.h"

#define SDHOST_BASE 0x3F202000

#define SDCMD   (*(volatile unsigned int*)(SDHOST_BASE + 0x00))
#define SDARG   (*(volatile unsigned int*)(SDHOST_BASE + 0x04))
#define SDTOUT  (*(volatile unsigned int*)(SDHOST_BASE + 0x08))
#define SDCDIV  (*(volatile unsigned int*)(SDHOST_BASE + 0x0C))
#define SDRSP0  (*(volatile unsigned int*)(SDHOST_BASE + 0x10))
#define SDHSTS  (*(volatile unsigned int*)(SDHOST_BASE + 0x20))
#define SDVDD   (*(volatile unsigned int*)(SDHOST_BASE + 0x30))
#define SDEDM   (*(volatile unsigned int*)(SDHOST_BASE + 0x34))
#define SDHCFG  (*(volatile unsigned int*)(SDHOST_BASE + 0x38))
#define SDHBCT  (*(volatile unsigned int*)(SDHOST_BASE + 0x3C))
#define SDDATA  (*(volatile unsigned int*)(SDHOST_BASE + 0x40))
#define SDHBLC  (*(volatile unsigned int*)(SDHOST_BASE + 0x50))

#define SDCMD_NEW_FLAG      0x8000
#define SDCMD_FAIL_FLAG     0x4000
#define SDCMD_NO_RESPONSE   0x0400
#define SDCMD_LONG_RESPONSE 0x0200
#define SDCMD_READ_CMD      0x0040

#define SDHSTS_DATA_FLAG        0x0001
#define SDHSTS_FIFO_ERROR       0x0008
#define SDHSTS_CRC7_ERROR       0x0010
#define SDHSTS_CRC16_ERROR      0x0020
#define SDHSTS_CMD_TIME_OUT     0x0040
#define SDHSTS_REW_TIME_OUT     0x0080
#define SDHSTS_CLEAR_MASK       0x07F8
#define SDHSTS_TRANSFER_ERRORS  (SDHSTS_FIFO_ERROR | SDHSTS_CRC16_ERROR | SDHSTS_REW_TIME_OUT)
#define SDHSTS_ERROR_MASK       (SDHSTS_CMD_TIME_OUT | SDHSTS_CRC7_ERROR | SDHSTS_TRANSFER_ERRORS)

#define SDEDM_FSM_MASK       0xF
#define SDEDM_FIFO_FILL_SHIFT 4
#define SDEDM_FIFO_FILL_MASK  0x1F
#define SDEDM_READ_THRESHOLD_SHIFT 14
#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_THRESHOLD_MASK 0x1F

#define CMD_NEEDS_RESP 1
#define CMD_LONG_RESP  2
#define CMD_IS_READ    4

static unsigned int sd_rca = 0;
static int sd_is_sdhc = 0;

static void delay(int count) {
    while (count--) asm volatile("nop");
}

static int wait_cmd_done(int timeout) {
    while ((SDCMD & SDCMD_NEW_FLAG) && timeout--) {
        barrier();
    }
    return timeout > 0 ? 0 : -1;
}

static void clear_status(void) {
    SDHSTS = SDHSTS_CLEAR_MASK;
    barrier();
}

static void sdhost_drain_fifo(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(SDHSTS & SDHSTS_DATA_FLAG)) {
            break;
        }
        (void)SDDATA;
        barrier();
    }
}

static int sdhost_ensure_transfer_state(void) {
    const unsigned int STATE_MASK = 0xF;
    const unsigned int STATE_SHIFT = 9;
    const unsigned int STATE_TRANSFER = 4;

    if (sd_rca == 0) {
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        if (sdhost_cmd(13, sd_rca << 16, CMD_NEEDS_RESP) == 0) {
            unsigned int st = sdhost_get_resp();
            unsigned int state = (st >> STATE_SHIFT) & STATE_MASK;
            unsigned int ready = (st >> 8) & 1;
            if (state == STATE_TRANSFER && ready) {
                return 0;
            }
        }
        if (sdhost_cmd(7, sd_rca << 16, CMD_NEEDS_RESP) == 0) {
            (void)sdhost_get_resp();
        }
    }

    return -1;
}

unsigned int sdhost_get_resp(void) {
    return SDRSP0;
}

void sdhost_reset(void) {
    SDVDD = 0;
    delay(10000);

    SDCMD = 0;
    SDARG = 0;
    SDTOUT = 0xF00000;
    SDCDIV = 0x000007FF;
    clear_status();

    unsigned int temp = SDEDM;
    temp &= ~((SDEDM_THRESHOLD_MASK << SDEDM_READ_THRESHOLD_SHIFT) |
              (SDEDM_THRESHOLD_MASK << SDEDM_WRITE_THRESHOLD_SHIFT));
    temp |= (4 << SDEDM_READ_THRESHOLD_SHIFT);
    temp |= (4 << SDEDM_WRITE_THRESHOLD_SHIFT);
    SDEDM = temp;

    SDHCFG = (1 << 0) | (1 << 1) | (1 << 3);

    delay(100000);
    SDVDD = 1;
    delay(500000);
}

int sdhost_cmd(unsigned int cmd, unsigned int arg, unsigned int flags) {
    if (wait_cmd_done(1000000) != 0) {
        return -1;
    }

    clear_status();

    unsigned int sdcmd = cmd;
    if (!(flags & CMD_NEEDS_RESP)) {
        sdcmd |= SDCMD_NO_RESPONSE;
    } else if (flags & CMD_LONG_RESP) {
        sdcmd |= SDCMD_LONG_RESPONSE;
    }
    if (flags & CMD_IS_READ) {
        sdcmd |= SDCMD_READ_CMD;
    }

    SDARG = arg;
    barrier();
    SDCMD = sdcmd | SDCMD_NEW_FLAG;

    if (wait_cmd_done(1000000) != 0) {
        return -1;
    }

    if (SDCMD & SDCMD_FAIL_FLAG) {
        return -1;
    }

    if (SDHSTS & SDHSTS_CMD_TIME_OUT) {
        clear_status();
        return -1;
    }

    return 0;
}

void sdhost_check_status(void) {
    if (sdhost_cmd(13, sd_rca << 16, CMD_NEEDS_RESP) == 0) {
        uart_puts("Status = ");
        uart_puthex(sdhost_get_resp());
        uart_puts("\n");
    }
}

int sdhost_init_card(void) {
    if (sdhost_cmd(0, 0, 0) != 0) return -1;
    delay(100000);

    int sd_v2 = 0;
    if (sdhost_cmd(8, 0x1AA, CMD_NEEDS_RESP) == 0) {
        unsigned int r = sdhost_get_resp();
        if ((r & 0xFFF) == 0x1AA) {
            sd_v2 = 1;
        }
    }

    unsigned int resp = 0;
    int retries = 2000;
    do {
        if (sdhost_cmd(55, 0, CMD_NEEDS_RESP) != 0) return -1;

        unsigned int acmd41_arg = sd_v2 ? 0x40300000 : 0x00300000;
        if (sdhost_cmd(41, acmd41_arg, CMD_NEEDS_RESP) != 0) return -1;

        resp = sdhost_get_resp();
        if (resp & 0x80000000) break;
        delay(10000);
    } while (--retries);

    if (retries == 0) return -1;

    sd_is_sdhc = (resp & (1 << 30)) ? 1 : 0;

    if (sdhost_cmd(2, 0, CMD_NEEDS_RESP | CMD_LONG_RESP) != 0) return -1;
    (void)sdhost_get_resp();

    if (sdhost_cmd(3, 0, CMD_NEEDS_RESP) != 0) return -1;
    unsigned int cmd3_resp = sdhost_get_resp();
    sd_rca = (cmd3_resp >> 16) & 0xFFFF;
    if (sd_rca == 0) return -1;

    if (sdhost_cmd(7, sd_rca << 16, CMD_NEEDS_RESP) != 0) return -1;
    (void)sdhost_get_resp();

    return sdhost_ensure_transfer_state();
}

static int sdhost_read_block_once(unsigned int lba, unsigned char *buffer) {
    unsigned int addr = sd_is_sdhc ? lba : (lba * 512);

    if (sdhost_ensure_transfer_state() != 0) {
        return -1;
    }

    sdhost_drain_fifo();
    clear_status();

    SDHBCT = 512;
    SDHBLC = 1;

    if (sdhost_cmd(17, addr, CMD_NEEDS_RESP | CMD_IS_READ) != 0) {
        return -1;
    }
    (void)sdhost_get_resp();

    int words_left = 128;
    int out_idx = 0;
    int timeout = 2000000;

    while (words_left > 0 && timeout-- > 0) {
        unsigned int status = SDHSTS;
        if (status & SDHSTS_TRANSFER_ERRORS) {
            clear_status();
            return -1;
        }

        unsigned int edm = SDEDM;
        unsigned int fifo_words = (edm >> SDEDM_FIFO_FILL_SHIFT) & SDEDM_FIFO_FILL_MASK;

        if (fifo_words == 0) {
            barrier();
            continue;
        }

        int burst = fifo_words;
        if (burst > words_left) burst = words_left;

        for (int i = 0; i < burst; i++) {
            unsigned int data = SDDATA;
            buffer[out_idx++] = (unsigned char)(data & 0xFF);
            buffer[out_idx++] = (unsigned char)((data >> 8) & 0xFF);
            buffer[out_idx++] = (unsigned char)((data >> 16) & 0xFF);
            buffer[out_idx++] = (unsigned char)((data >> 24) & 0xFF);
            words_left--;
        }
    }

    if (words_left != 0) {
        clear_status();
        return -1;
    }

    if (SDHSTS & SDHSTS_TRANSFER_ERRORS) {
        clear_status();
        return -1;
    }

    int settle = 200000;
    while (settle-- > 0) {
        unsigned int fsm = SDEDM & SDEDM_FSM_MASK;
        if (fsm == 0 && !(SDHSTS & SDHSTS_DATA_FLAG)) {
            break;
        }
        barrier();
    }

    clear_status();
    return 0;
}

int sdhost_read_block(unsigned int lba, unsigned char *buffer) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (sdhost_read_block_once(lba, buffer) == 0) {
            return 0;
        }

        if (sd_rca != 0) {
            if (sdhost_cmd(7, sd_rca << 16, CMD_NEEDS_RESP) == 0) {
                (void)sdhost_get_resp();
            }
        }

        delay(20000);
    }

    return -1;
}
