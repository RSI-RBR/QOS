#include "sdio_bus.h"
#include "gpio.h"
#include "mailbox.h"
#include "platform/mmio.h"
#include "timer.h"
#include "uart.h"
#include "debug.h"
#include "clock.h"

#define EMMC_BASE QOS_EMMC_BASE

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
#define INT_READ_RDY      (1u << 5)
#define INT_WRITE_RDY     (1u << 4)
#define INT_ERR           (1u << 15)
#define INT_ERROR_MASK    0xFFFF0000u

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
#define CMD_RSPNS_48      (2u << 16)
#define CMD_RSPNS_48B     (3u << 16)
#define CMD_CRCCHK_EN     (1u << 19)
#define CMD_IXCHK_EN      (1u << 20)
#define CMD_ISDATA        (1u << 21)

#define TM_BLKCNT_EN      (1u << 1)
#define TM_DAT_DIR_CH     (1u << 4)

#define SDIO_CCCR_IOEX      0x02u
#define SDIO_CCCR_IORX      0x03u
#define SDIO_CCCR_INT_EN    0x04u
#define SDIO_CCCR_BUS_CTRL  0x07u
#define SDIO_CCCR_BLKSIZE   0x10u
#define SDIO_CCCR_HIGHSPEED 0x13u
#define SDIO_FBR1_BASE      0x100u
#define SDIO_FBR2_BASE      0x200u
#define SDIO_OCR_33V_MASK   (3u << 20) // Circle ether4330: V3_3, 3.2-3.4V

static int g_ready = 0;
static unsigned short g_rca = 0;
static unsigned int g_ocr = 0;

static void sdio_short_delay(unsigned int n){
    while (n--){
        asm volatile("nop");
    }
}

static unsigned long sdio_read_cntpct(void){
    unsigned long v = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static unsigned long sdio_read_cntfrq(void){
    unsigned long v = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void sdio_clear_interrupts(void){
    EMMC_INTERRUPT = 0xFFFFFFFFu;
}

static int sdio_wait_status_clear(unsigned int mask, unsigned long timeout_ms){
    unsigned long start = system_ticks;
    unsigned int spin = 200000u;
    while (EMMC_STATUS & mask){
        if ((system_ticks - start) > timeout_ms || --spin == 0u){
            return -1;
        }
    }
    return 0;
}

static int sdio_wait_irq(unsigned int mask, unsigned long timeout_ms, unsigned int* irpt_out){
    unsigned long start = system_ticks;
    unsigned int spin = 10000000u;
    while (1){
        unsigned int irpt = EMMC_INTERRUPT;
        if (irpt & (mask | INT_ERROR_MASK | INT_ERR)){
            if (irpt_out){
                *irpt_out = irpt;
            }
            return 0;
        }
        if ((system_ticks - start) > timeout_ms || --spin == 0u){
            if (irpt_out){
                *irpt_out = EMMC_INTERRUPT;
            }
            return -1;
        }
    }
}

static int sdio_reset_lines(void){
    EMMC_CONTROL1 |= C1_SRST_CMD | C1_SRST_DAT;
    {
        unsigned long start = system_ticks;
        unsigned int spin = 200000u;
        while (EMMC_CONTROL1 & (C1_SRST_CMD | C1_SRST_DAT)){
            if ((system_ticks - start) > 120 || --spin == 0u){
                return -1;
            }
        }
    }
    return 0;
}

static int sdio_cmd(unsigned int cmd, unsigned int arg, unsigned int flags, unsigned long timeout_ms){
    unsigned int irpt = 0;

    if (sdio_wait_status_clear(SR_CMD_INHIBIT, 120) != 0){
        EMMC_CONTROL1 |= C1_SRST_CMD;
        sdio_short_delay(10000);
        return -1;
    }
    if ((flags & CMD_ISDATA) || ((flags & CMD_RSPNS_48B) == CMD_RSPNS_48B)){
        if (sdio_wait_status_clear(SR_DAT_INHIBIT, 120) != 0){
            return -1;
        }
    }

    sdio_clear_interrupts();
    EMMC_ARG1 = arg;
    EMMC_CMDTM = (cmd << 24) | flags;

    if (sdio_wait_irq(INT_CMD_DONE, timeout_ms, &irpt) != 0){
        return -1;
    }
    EMMC_INTERRUPT = (irpt & (INT_CMD_DONE | INT_ERROR_MASK | INT_ERR));
    if (irpt & (INT_ERROR_MASK | INT_ERR)){
        return -1;
    }
    return 0;
}

static int sdio_cmd52(int write, unsigned int fn, unsigned int addr, unsigned char in, unsigned char* out){
    unsigned int arg = 0;

    if (fn > 7u || addr > 0x1FFFFu){
        return -1;
    }

    if (write){
        arg |= (1u << 31);
    }
    arg |= ((fn & 0x7u) << 28);
    arg |= ((addr & 0x1FFFFu) << 9);
    arg |= (unsigned int)in;

    if (sdio_cmd(52, arg, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
        return -1;
    }
    if (out){
        *out = (unsigned char)(EMMC_RESP0 & 0xFFu);
    }
    return 0;
}

static void sdio_fn0_set_bits(unsigned int addr, unsigned char bits){
    unsigned char val = 0;
    if (sdio_cmd52(0, 0, addr, 0, &val) == 0){
        (void)sdio_cmd52(1, 0, addr, (unsigned char)(val | bits), 0);
    }
}

static void sdio_set_block_size(unsigned int fn, unsigned int size){
    unsigned int base = (fn == 1u) ? SDIO_FBR1_BASE : SDIO_FBR2_BASE;
    (void)sdio_cmd52(1, 0, base + SDIO_CCCR_BLKSIZE, (unsigned char)(size & 0xFFu), 0);
    (void)sdio_cmd52(1, 0, base + SDIO_CCCR_BLKSIZE + 1u, (unsigned char)((size >> 8) & 0xFFu), 0);
}

static int sdio_cmd53_xfer(int write, unsigned int fn, unsigned int addr,
                           unsigned char* buf, unsigned int len, int incr){
    unsigned int done = 0;
    while (done < len){
        unsigned int chunk = len - done;
        unsigned int irpt = 0;
        unsigned int arg = 0;
        unsigned int words = 0;
        if (chunk > 512u){
            chunk = 512u;
        }

        EMMC_BLKSIZECNT = (1u << 16) | chunk;

        if (write){
            arg |= (1u << 31);
        }
        arg |= ((fn & 0x7u) << 28);
        // byte mode (bit27=0). Backplane accesses increment the address;
        // SDPCM packet I/O uses a fixed Function 2 address like Circle.
        if (incr){
            arg |= (1u << 26);
        }
        arg |= ((addr + (incr ? done : 0u)) & 0x1FFFFu) << 9;
        // Byte mode count: 0 means 512 bytes.
        arg |= (chunk == 512u) ? 0u : (chunk & 0x1FFu);

        if (sdio_cmd(53, arg,
                     CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA |
                     (write ? 0u : TM_DAT_DIR_CH) | TM_BLKCNT_EN,
                     200) != 0){
            return -1;
        }

        if (sdio_wait_irq(write ? INT_WRITE_RDY : INT_READ_RDY, 200, &irpt) != 0){
            return -1;
        }
        EMMC_INTERRUPT = (irpt & ((write ? INT_WRITE_RDY : INT_READ_RDY) | INT_ERROR_MASK | INT_ERR));
        if (irpt & (INT_ERROR_MASK | INT_ERR)){
            return -1;
        }

        words = (chunk + 3u) / 4u;
        for (unsigned int i = 0; i < words; i++){
            if (write){
                unsigned int idx = done + (i * 4u);
                unsigned int d = 0;
                if (idx + 0u < len) d |= (unsigned int)buf[idx + 0u];
                if (idx + 1u < len) d |= ((unsigned int)buf[idx + 1u] << 8);
                if (idx + 2u < len) d |= ((unsigned int)buf[idx + 2u] << 16);
                if (idx + 3u < len) d |= ((unsigned int)buf[idx + 3u] << 24);
                EMMC_DATA = d;
            } else{
                unsigned int idx = done + (i * 4u);
                unsigned int d = EMMC_DATA;
                if (idx + 0u < len) buf[idx + 0u] = (unsigned char)(d & 0xFFu);
                if (idx + 1u < len) buf[idx + 1u] = (unsigned char)((d >> 8) & 0xFFu);
                if (idx + 2u < len) buf[idx + 2u] = (unsigned char)((d >> 16) & 0xFFu);
                if (idx + 3u < len) buf[idx + 3u] = (unsigned char)((d >> 24) & 0xFFu);
            }
        }

        if (sdio_wait_irq(INT_DATA_DONE, 200, &irpt) != 0){
            return -1;
        }
        EMMC_INTERRUPT = (irpt & (INT_DATA_DONE | INT_ERROR_MASK | INT_ERR));
        if (irpt & (INT_ERROR_MASK | INT_ERR)){
            return -1;
        }

        if (sdio_wait_status_clear(SR_DAT_INHIBIT, 120) != 0){
            return -1;
        }
        done += chunk;
    }

    return 0;
}

int sdio_bus_is_ready(void){
    return g_ready;
}

unsigned short sdio_bus_get_rca(void){
    return g_rca;
}

void sdio_bus_reset_state(void){
    g_ready = 0;
    g_rca = 0;
    g_ocr = 0;
}

void sdio_bus_suspend_state(void){
    g_ready = 0;
}

static int sdio_bus_init_common(int pulse_wl_on, int live_reattach){
    unsigned int resp = 0;
    unsigned int c1 = 0;
    unsigned int irpt = 0;
    unsigned short saved_rca = g_rca;

    g_ready = 0;
    if (!live_reattach){
        g_rca = 0;
        g_ocr = 0;
    }

    // Circle's ether4330 first disconnects EMMC from the SD-card pins
    // (GPIO48..53 ALT0), then connects EMMC to WiFi SDIO on GPIO34..39 ALT3.
    gpio_init_sd();
    gpio_init_wifi_sdio();
    clock_init_wifi_lpo();
    if (pulse_wl_on){
        gpio_wifi_wl_on_pulse();
    }
    mailbox_set_emmc_clock(25000000);
    uart_puts("SDIO: stage host reset\n");

    EMMC_CONTROL1 |= C1_SRST_HC;
    {
        unsigned long start = system_ticks;
        unsigned int spin = 200000u;
        while (EMMC_CONTROL1 & C1_SRST_HC){
            if ((system_ticks - start) > 200 || --spin == 0u){
                uart_puts("SDIO: host reset timeout\n");
                return -1;
            }
        }
    }

    uart_puts("SDIO: stage power/clock\n");
    EMMC_CONTROL0 = C0_SD_BUS_VOLT_33 | C0_SD_BUS_POWER;
    sdio_short_delay(50000);

    c1 = EMMC_CONTROL1;
    c1 &= ~(C1_CLK_DIV_MASK | C1_DATA_TOUNIT_MASK | C1_CLK_EN);
    c1 |= C1_CLK_INTLEN | C1_DATA_TOUNIT_MAX;
    // Conservative identification clock.
    c1 |= (128u << 8);
    EMMC_CONTROL1 = c1;

    {
        unsigned long start = system_ticks;
        unsigned int spin = 200000u;
        while (!(EMMC_CONTROL1 & C1_CLK_STABLE)){
            if ((system_ticks - start) > 200 || --spin == 0u){
                uart_puts("SDIO: clk stable timeout\n");
                return -1;
            }
        }
    }
    EMMC_CONTROL1 |= C1_CLK_EN;

    uart_puts("SDIO: stage reset lines\n");
    if (sdio_reset_lines() != 0){
        uart_puts("SDIO: line reset failed\n");
        return -1;
    }

    sdio_clear_interrupts();
    EMMC_IRPT_MASK = 0xFFFFFFFFu;
    EMMC_IRPT_EN = 0xFFFFFFFFu;

    if (live_reattach && saved_rca != 0u){
        g_rca = saved_rca;
        uart_puts("SDIO: stage live reselect\n");
        if (sdio_cmd(7, (unsigned int)g_rca << 16, CMD_RSPNS_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
            uart_puts("SDIO: live reselect failed\n");
            return -1;
        }

        g_ready = 1;
        EMMC_CONTROL0 |= C0_HCTL_DWIDTH;
        sdio_fn0_set_bits(SDIO_CCCR_HIGHSPEED, 0x02u);
        sdio_fn0_set_bits(SDIO_CCCR_BUS_CTRL, 0x02u);
        sdio_set_block_size(1, 64u);
        sdio_set_block_size(2, 512u);
        (void)sdio_cmd52(1, 0, SDIO_CCCR_INT_EN, 0x00u, 0);
        uart_puts("SDIO: live reattach OK\n");
        return 0;
    }

    uart_puts("SDIO: stage CMD0\n");
    if (sdio_cmd(0, 0, CMD_RSPNS_NONE, 120) != 0){
        uart_puts("SDIO: CMD0 failed\n");
        return -1;
    }
    (void)sdio_reset_lines();
    sdio_clear_interrupts();
    // SDIO cards are initialized with CMD5 (not ACMD41/CMD8 flow).
    // Some chips may not respond sanely to CMD8 during SDIO bring-up.

    // SDIO OCR negotiation via CMD5.
    uart_puts("SDIO: stage CMD5\n");
    (void)sdio_cmd(5, 0, CMD_RSPNS_48, 120);
    resp = EMMC_RESP0;

    for (int i = 0; i < 8; i++){
        if (resp & 0x80000000u){
            g_ocr = resp;
            break;
        }

        // Circle retries CMD5 with only the 3.2-3.4V OCR window and a real settle gap.
        if (sdio_cmd(5, SDIO_OCR_33V_MASK, CMD_RSPNS_48, 120) != 0){
            sdio_short_delay(10000000u);
            continue;
        }
        resp = EMMC_RESP0;
        if (resp & 0x80000000u){
            g_ocr = resp;
            break;
        }
        sdio_short_delay(10000000u);
    }
    if ((resp & 0x80000000u) == 0){
        uart_puts("SDIO: CMD5 power-up timeout\n");
        uart_puts("SDIO: RESP0=");
        uart_puthex(EMMC_RESP0);
        uart_puts(" STATUS=");
        uart_puthex(EMMC_STATUS);
        uart_puts(" IRPT=");
        irpt = EMMC_INTERRUPT;
        uart_puthex(irpt);
        uart_puts(" C1=");
        uart_puthex(EMMC_CONTROL1);
        uart_puts(" C0=");
        uart_puthex(EMMC_CONTROL0);
        uart_puts("\n");
        return -1;
    }

    if (sdio_cmd(3, 0, CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
        uart_puts("SDIO: CMD3 failed\n");
        return -1;
    }
    g_rca = (unsigned short)((EMMC_RESP0 >> 16) & 0xFFFFu);
    if (g_rca == 0){
        uart_puts("SDIO: invalid RCA\n");
        return -1;
    }

    if (sdio_cmd(7, (unsigned int)g_rca << 16, CMD_RSPNS_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN, 120) != 0){
        uart_puts("SDIO: CMD7 select failed\n");
        return -1;
    }

    // Request 4-bit bus mode via CCCR bus control register.
    sdio_fn0_set_bits(SDIO_CCCR_HIGHSPEED, 0x02u);
    sdio_fn0_set_bits(SDIO_CCCR_BUS_CTRL, 0x02u);
    EMMC_CONTROL0 |= C0_HCTL_DWIDTH;

    // Circle programs the function block sizes before enabling Function 1.
    sdio_set_block_size(1, 64u);
    sdio_set_block_size(2, 512u);
    (void)sdio_cmd52(1, 0, SDIO_CCCR_INT_EN, 0x00u, 0);

    g_ready = 1;
    uart_puts("SDIO: init OK\n");
    return 0;
}

int sdio_bus_init(void){
    return sdio_bus_init_common(1, 0);
}

int sdio_bus_reattach(void){
    return sdio_bus_init_common(0, 1);
}

int sdio_bus_cmd52_read(unsigned int fn, unsigned int addr, unsigned char* out_val){
    if (!g_ready){
        return -1;
    }
    return sdio_cmd52(0, fn, addr, 0, out_val);
}

int sdio_bus_cmd52_write(unsigned int fn, unsigned int addr, unsigned char val){
    if (!g_ready){
        return -1;
    }
    return sdio_cmd52(1, fn, addr, val, 0);
}

int sdio_bus_cmd53_read(unsigned int fn, unsigned int addr, unsigned char* out, unsigned int len){
    if (!g_ready || !out || len == 0){
        return -1;
    }
    return sdio_cmd53_xfer(0, fn, addr, out, len, 1);
}

int sdio_bus_cmd53_write(unsigned int fn, unsigned int addr, const unsigned char* data, unsigned int len){
    if (!g_ready || !data || len == 0){
        return -1;
    }
    // Internal xfer helper expects mutable pointer but does not mutate write input.
    return sdio_cmd53_xfer(1, fn, addr, (unsigned char*)data, len, 1);
}

int sdio_bus_cmd53_read_fixed(unsigned int fn, unsigned int addr, unsigned char* out, unsigned int len){
    if (!g_ready || !out || len == 0){
        return -1;
    }
    return sdio_cmd53_xfer(0, fn, addr, out, len, 0);
}

int sdio_bus_cmd53_write_fixed(unsigned int fn, unsigned int addr, const unsigned char* data, unsigned int len){
    if (!g_ready || !data || len == 0){
        return -1;
    }
    // Internal xfer helper expects mutable pointer but does not mutate write input.
    return sdio_cmd53_xfer(1, fn, addr, (unsigned char*)data, len, 0);
}

int sdio_bus_enable_func(unsigned int fn){
    unsigned char ioex = 0;
    unsigned char wanted = 0;
    int rc = 0;
    if (fn == 0 || fn > 7){
        return -1;
    }
    if (sdio_bus_cmd52_read(0, SDIO_CCCR_IOEX, &ioex) != 0){
        uart_puts("SDIO: IOEX read failed fn=");
        uart_putdec(fn);
        uart_puts(" status=");
        uart_puthex(EMMC_STATUS);
        uart_puts(" irpt=");
        uart_puthex(EMMC_INTERRUPT);
        uart_puts("\n");
        return -1;
    }
    wanted = (unsigned char)(ioex | (1u << fn));
    uart_puts("SDIO: enable fn=");
    uart_putdec(fn);
    uart_puts(" ioex=");
    uart_puthex(ioex);
    uart_puts(" -> ");
    uart_puthex(wanted);
    uart_puts("\n");
    rc = sdio_bus_cmd52_write(0, SDIO_CCCR_IOEX, wanted);
    if (rc != 0){
        uart_puts("SDIO: IOEX write failed fn=");
        uart_putdec(fn);
        uart_puts(" status=");
        uart_puthex(EMMC_STATUS);
        uart_puts(" irpt=");
        uart_puthex(EMMC_INTERRUPT);
        uart_puts("\n");
    }
    return rc;
}

int sdio_bus_wait_func_ready(unsigned int fn, unsigned int timeout_ms){
    unsigned long freq = sdio_read_cntfrq();
    unsigned long start = sdio_read_cntpct();
    unsigned long timeout_cycles = 0;
    unsigned int polls = 0;
    unsigned char iorx = 0;
    unsigned char last_iorx = 0;
    if (fn == 0 || fn > 7){
        return -1;
    }
    if (timeout_ms == 0u){
        timeout_ms = 1u;
    }
    timeout_cycles = (freq / 1000u) * (unsigned long)timeout_ms;
    if (timeout_cycles == 0u){
        timeout_cycles = freq / 1000u;
    }
    /*
     * Do not cap this to a tiny fixed poll count. Function 2 can legitimately
     * take hundreds of milliseconds to report ready after CYW43 firmware boot,
     * especially on Pi Zero-class boards and with patched monitor firmware.
     */
    while ((unsigned long)(sdio_read_cntpct() - start) <= timeout_cycles){
        if (sdio_bus_cmd52_read(0, SDIO_CCCR_IORX, &iorx) == 0){
            last_iorx = iorx;
            if (iorx & (1u << fn)){
                uart_puts("SDIO: fn ready fn=");
                uart_putdec(fn);
                uart_puts(" iorx=");
                uart_puthex(iorx);
                uart_puts("\n");
                return 0;
            }
        } else if (polls == 0u || polls == 10u || polls == 50u || polls == 200u){
            uart_puts("SDIO: IORX read fail fn=");
            uart_putdec(fn);
            uart_puts(" i=");
            uart_putdec(polls);
            uart_puts(" status=");
            uart_puthex(EMMC_STATUS);
            uart_puts(" irpt=");
            uart_puthex(EMMC_INTERRUPT);
            uart_puts("\n");
        }
        if ((polls == 0u || polls == 10u || polls == 50u ||
             polls == 200u || polls == 1000u) && last_iorx){
            uart_puts("SDIO: wait fn=");
            uart_putdec(fn);
            uart_puts(" iorx=");
            uart_puthex(last_iorx);
            uart_puts("\n");
        }
        polls++;
        sdio_short_delay(50000u);
    }
    uart_puts("SDIO: fn ready timeout fn=");
    uart_putdec(fn);
    uart_puts(" iorx=");
    uart_puthex(last_iorx);
    uart_puts(" status=");
    uart_puthex(EMMC_STATUS);
    uart_puts(" irpt=");
    uart_puthex(EMMC_INTERRUPT);
    uart_puts("\n");
    return -1;
}
