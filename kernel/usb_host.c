#include "usb_host.h"
#include "uart.h"

// Raspberry Pi USB OTG (DWC2) base on Pi 2/3 peripheral map.
#define USB_DWC2_BASE 0x3F980000UL

#define GOTGCTL   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x000))
#define GAHBCFG   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x008))
#define GUSBCFG   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x00C))
#define GRSTCTL   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x010))
#define GINTSTS   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x014))
#define GINTMSK   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x018))
#define GRXSTSP   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x020))
#define GRXFSIZ   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x024))
#define GNPTXFSIZ (*(volatile unsigned int*)(USB_DWC2_BASE + 0x028))
#define GCCFG     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x038))
#define GSNPSID   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x040))
#define HCFG      (*(volatile unsigned int*)(USB_DWC2_BASE + 0x400))
#define HFIR      (*(volatile unsigned int*)(USB_DWC2_BASE + 0x404))
#define HFNUM     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x408))
#define HPRT0     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x440))
#define PCGCTL    (*(volatile unsigned int*)(USB_DWC2_BASE + 0xE00))
#define FIFO0     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x1000))

#define HC_REG_BASE(ch)   (USB_DWC2_BASE + 0x500 + ((ch) * 0x20))
#define HCCHAR(ch)        (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x00))
#define HCINT(ch)         (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x08))
#define HCINTMSK(ch)      (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x0C))
#define HCTSIZ(ch)        (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x10))

// GRSTCTL bits
#define GRSTCTL_CSRST        (1u << 0)
#define GRSTCTL_AHB_IDLE     (1u << 31)

// GUSBCFG bits
#define GUSBCFG_FHMOD        (1u << 29)
#define GUSBCFG_FDMOD        (1u << 30)

// GAHBCFG bits
#define GAHBCFG_GLBL_INTR_EN (1u << 0)

// HPRT0 bits
#define HPRT0_PWR            (1u << 12)
#define HPRT0_CONN_STS       (1u << 0)
#define HPRT0_CONN_DET       (1u << 1)
#define HPRT0_ENA            (1u << 2)
#define HPRT0_ENA_CHG        (1u << 3)
#define HPRT0_OVRCURR        (1u << 4)
#define HPRT0_OVRCURR_CHG    (1u << 5)
#define HPRT0_RESET          (1u << 8)
#define HPRT0_SPD_MASK       (3u << 17)
#define HPRT0_SPD_HIGH       (0u << 17)
#define HPRT0_SPD_FULL       (1u << 17)
#define HPRT0_SPD_LOW        (2u << 17)

// GINTSTS bits (subset)
#define GINTSTS_CURMODE_HOST (1u << 0)
#define GINTSTS_RXFLVL       (1u << 4)

// Host channel bits
#define HCCHAR_MPS_MASK      0x7FFu
#define HCCHAR_EPNUM_SHIFT   11
#define HCCHAR_EPDIR         (1u << 15)
#define HCCHAR_LSPDDEV       (1u << 17)
#define HCCHAR_EPTYPE_SHIFT  18
#define HCCHAR_DEVADDR_SHIFT 22
#define HCCHAR_ODDFRM        (1u << 29)
#define HCCHAR_CHDIS         (1u << 30)
#define HCCHAR_CHENA         (1u << 31)

// HCINT bits
#define HCINT_XFERCOMPL      (1u << 0)
#define HCINT_CHHLTD         (1u << 1)
#define HCINT_AHBERR         (1u << 2)
#define HCINT_STALL          (1u << 3)
#define HCINT_NAK            (1u << 4)
#define HCINT_ACK            (1u << 5)
#define HCINT_NYET           (1u << 6)
#define HCINT_XACTERR        (1u << 7)
#define HCINT_BBLERR         (1u << 8)
#define HCINT_FRMOVRUN       (1u << 9)
#define HCINT_DATATGLERR     (1u << 10)
#define HCINT_ERROR_MASK     (HCINT_AHBERR | HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)

// HCTSIZ bits
#define HCTSIZ_XFERSIZE_MASK 0x7FFFFu
#define HCTSIZ_PKTCNT_SHIFT  19
#define HCTSIZ_PID_SHIFT     29
#define HCTSIZ_PID_DATA0     0u
#define HCTSIZ_PID_DATA1     2u
#define HCTSIZ_PID_SETUP     3u

// GRXSTSP fields
#define GRXSTSP_CHNUM_MASK   0xFu
#define GRXSTSP_BCNT_SHIFT   4
#define GRXSTSP_BCNT_MASK    (0x7FFu << GRXSTSP_BCNT_SHIFT)
#define GRXSTSP_PKTSTS_SHIFT 17
#define GRXSTSP_PKTSTS_MASK  (0xFu << GRXSTSP_PKTSTS_SHIFT)
#define GRXSTSP_PKTSTS_IN    0x2u

#define USB_CTRL_EP_MPS_DEFAULT 8u

static int g_usb_ready = 0;
static unsigned int g_port_speed = HPRT0_SPD_FULL;
static unsigned int g_ep0_mps = USB_CTRL_EP_MPS_DEFAULT;

static void spin_delay(unsigned int n){
    while (n--){
        asm volatile("nop");
    }
}

static int wait_mask_set(volatile unsigned int* reg, unsigned int mask, unsigned int loops){
    while (loops--){
        if ((*reg) & mask){
            return 0;
        }
    }
    return -1;
}

static int wait_mask_clear(volatile unsigned int* reg, unsigned int mask, unsigned int loops){
    while (loops--){
        if (((*reg) & mask) == 0){
            return 0;
        }
    }
    return -1;
}

static int wait_host_mode(unsigned int loops){
    while (loops--){
        if (GINTSTS & GINTSTS_CURMODE_HOST){
            return 0;
        }
    }
    return -1;
}

static int wait_port_connect(unsigned int loops){
    while (loops--){
        if (HPRT0 & HPRT0_CONN_STS){
            return 0;
        }
    }
    return -1;
}

static unsigned int div_round_up(unsigned int n, unsigned int d){
    return (n + d - 1u) / d;
}

static void hc_force_halt(unsigned int ch){
    unsigned int hcchar = HCCHAR(ch);
    hcchar |= (HCCHAR_CHDIS | HCCHAR_CHENA);
    HCCHAR(ch) = hcchar;

    for (unsigned int i = 0; i < 200000; i++){
        if ((HCCHAR(ch) & HCCHAR_CHENA) == 0){
            break;
        }
    }
}

static int hc_wait_idle(unsigned int ch, unsigned int loops){
    while (loops--){
        if ((HCCHAR(ch) & HCCHAR_CHENA) == 0){
            return 0;
        }
    }
    return -1;
}

static void clear_port_change_bits(void){
    unsigned int hprt = HPRT0;
    hprt &= ~(HPRT0_ENA | HPRT0_CONN_DET | HPRT0_ENA_CHG | HPRT0_OVRCURR_CHG | HPRT0_RESET);
    hprt |= HPRT0_PWR;
    hprt |= (HPRT0_CONN_DET | HPRT0_ENA_CHG | HPRT0_OVRCURR_CHG);
    HPRT0 = hprt;
}

static unsigned int hprt_base_for_write(void){
    unsigned int hprt = HPRT0;
    // Per DWC2 handling: mask write-sensitive bits before composing a write.
    hprt &= ~(HPRT0_ENA | HPRT0_CONN_DET | HPRT0_ENA_CHG | HPRT0_OVRCURR_CHG);
    return hprt;
}

static void fifo_write_bytes(const unsigned char* data, unsigned int len){
    unsigned int words = div_round_up(len, 4);
    for (unsigned int i = 0; i < words; i++){
        unsigned int w = 0;
        unsigned int base = i * 4;
        if (base + 0 < len) w |= (unsigned int)data[base + 0];
        if (base + 1 < len) w |= (unsigned int)data[base + 1] << 8;
        if (base + 2 < len) w |= (unsigned int)data[base + 2] << 16;
        if (base + 3 < len) w |= (unsigned int)data[base + 3] << 24;
        FIFO0 = w;
    }
}

static void fifo_read_bytes(unsigned char* data, unsigned int len){
    unsigned int words = div_round_up(len, 4);
    for (unsigned int i = 0; i < words; i++){
        unsigned int w = FIFO0;
        unsigned int base = i * 4;
        if (base + 0 < len) data[base + 0] = (unsigned char)(w & 0xFFu);
        if (base + 1 < len) data[base + 1] = (unsigned char)((w >> 8) & 0xFFu);
        if (base + 2 < len) data[base + 2] = (unsigned char)((w >> 16) & 0xFFu);
        if (base + 3 < len) data[base + 3] = (unsigned char)((w >> 24) & 0xFFu);
    }
}

// Return codes:
//  0 = completed
//  1 = retry suggested (NAK/NYET transient or halted mid-transaction)
// -1 = hard error
static int hc_wait_for_done(unsigned int ch, int is_in, unsigned char* in_buf, unsigned int in_len){
    unsigned int copied = 0;
    unsigned int loops = 8000000;

    while (loops--){
        if (is_in && (GINTSTS & GINTSTS_RXFLVL)){
            unsigned int rxst = GRXSTSP;
            unsigned int rx_ch = rxst & GRXSTSP_CHNUM_MASK;
            unsigned int pktsts = (rxst & GRXSTSP_PKTSTS_MASK) >> GRXSTSP_PKTSTS_SHIFT;
            unsigned int bcnt = (rxst & GRXSTSP_BCNT_MASK) >> GRXSTSP_BCNT_SHIFT;

            if (rx_ch == ch && pktsts == GRXSTSP_PKTSTS_IN && bcnt > 0 && in_buf){
                unsigned int room = (copied < in_len) ? (in_len - copied) : 0;
                unsigned int take = (bcnt < room) ? bcnt : room;
                if (take > 0){
                    fifo_read_bytes(&in_buf[copied], take);
                    copied += take;
                    // Drain remainder if packet larger than target buffer.
                    if (bcnt > take){
                        unsigned int skip_words = div_round_up(bcnt - take, 4);
                        for (unsigned int i = 0; i < skip_words; i++){
                            (void)FIFO0;
                        }
                    }
                } else{
                    // No room left: drain packet.
                    unsigned int skip_words = div_round_up(bcnt, 4);
                    for (unsigned int i = 0; i < skip_words; i++){
                        (void)FIFO0;
                    }
                }
            } else if (bcnt > 0){
                // Unexpected packet; drain to keep RX FIFO consistent.
                unsigned int skip_words = div_round_up(bcnt, 4);
                for (unsigned int i = 0; i < skip_words; i++){
                    (void)FIFO0;
                }
            }
        }

        unsigned int hcint = HCINT(ch);
        if (hcint & HCINT_ERROR_MASK){
            HCINT(ch) = hcint;
            return -1;
        }

        // ACK can occur before final completion on some control paths.
        // Treat it as progress, not a terminal retry condition.
        if (hcint & HCINT_ACK){
            HCINT(ch) = HCINT_ACK;
            if (hcint & HCINT_CHHLTD){
                return 0;
            }
            continue;
        }

        if (hcint & HCINT_XFERCOMPL){
            HCINT(ch) = hcint;
            return 0;
        }

        if (hcint & (HCINT_NAK | HCINT_NYET)){
            HCINT(ch) = (hcint & (HCINT_NAK | HCINT_NYET));
            return 1;
        }

        if (hcint & HCINT_CHHLTD){
            HCINT(ch) = HCINT_CHHLTD;
            return 1;
        }
    }
    return -1;
}

static int hc_transfer(unsigned int ch,
                       unsigned char dev_addr,
                       int ep_in,
                       unsigned int ep_mps,
                       unsigned int pid,
                       const unsigned char* out_data,
                       unsigned int out_len,
                       unsigned char* in_data,
                       unsigned int in_len){
    unsigned int xfer_len = ep_in ? in_len : out_len;
    unsigned int pktcnt = (xfer_len == 0) ? 1u : div_round_up(xfer_len, ep_mps ? ep_mps : 1u);
    if (pktcnt == 0){
        pktcnt = 1;
    }

    // Ensure channel is idle before programming a new transfer.
    if (hc_wait_idle(ch, 500000) != 0){
        hc_force_halt(ch);
        if (hc_wait_idle(ch, 500000) != 0){
            uart_puts("USB: HC not idle before xfer\n");
            return -1;
        }
    }

    for (unsigned int attempt = 0; attempt < 128; attempt++){
        if (attempt > 0){
            hc_force_halt(ch);
            if (hc_wait_idle(ch, 500000) != 0){
                return -1;
            }
        }
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_ERROR_MASK | HCINT_NAK | HCINT_ACK | HCINT_NYET;

        unsigned int hctsiz = (xfer_len & HCTSIZ_XFERSIZE_MASK)
            | (pktcnt << HCTSIZ_PKTCNT_SHIFT)
            | ((pid & 0x3u) << HCTSIZ_PID_SHIFT);
        HCTSIZ(ch) = hctsiz;

        unsigned int hcchar = (ep_mps & HCCHAR_MPS_MASK)
            | ((0u & 0xFu) << HCCHAR_EPNUM_SHIFT)
            | ((dev_addr & 0x7Fu) << HCCHAR_DEVADDR_SHIFT)
            | (0u << HCCHAR_EPTYPE_SHIFT); // control
        if (ep_in){
            hcchar |= HCCHAR_EPDIR;
        }
        if (g_port_speed == HPRT0_SPD_LOW){
            hcchar |= HCCHAR_LSPDDEV;
        }
        if (HFNUM & 1u){
            hcchar |= HCCHAR_ODDFRM;
        }

        HCCHAR(ch) = hcchar;

        if (!ep_in && out_len > 0 && out_data){
            fifo_write_bytes(out_data, out_len);
        }

        hcchar |= HCCHAR_CHENA;
        hcchar &= ~HCCHAR_CHDIS;
        HCCHAR(ch) = hcchar;

        int rc = hc_wait_for_done(ch, ep_in, in_data, in_len);
        if (rc == 0){
            // Do not force-halt on success; it can race the next control stage.
            return 0;
        }
        if (rc < 0){
            uart_puts("USB: hc xfer hard fail hcint=");
            uart_puthex(HCINT(ch));
            uart_puts(" hctsiz=");
            uart_puthex(HCTSIZ(ch));
            uart_puts(" hcchar=");
            uart_puthex(HCCHAR(ch));
            uart_puts(" hprt0=");
            uart_puthex(HPRT0);
            uart_puts("\n");
            hc_force_halt(ch);
            return -1;
        }
        // Transient NAK/NYET/halt: short settle then retry.
        hc_force_halt(ch);
        spin_delay(30000);
    }

    uart_puts("USB: hc xfer retry exhausted\n");
    return -1;
}

int usb_host_reset_root_port(void){
    if (!(HPRT0 & HPRT0_CONN_STS)){
        uart_puts("USB: reset requested with no connected device.\n");
        return -1;
    }

    for (unsigned int attempt = 0; attempt < 4; attempt++){
        unsigned int hprt = hprt_base_for_write();
        hprt |= HPRT0_PWR | HPRT0_RESET;
        HPRT0 = hprt;

        // Keep reset asserted long enough for HS negotiation.
        spin_delay(50000000);

        hprt = hprt_base_for_write();
        hprt |= HPRT0_PWR;
        hprt &= ~HPRT0_RESET;
        HPRT0 = hprt;

        // Let port settle and enable latch.
        spin_delay(4000000);
        clear_port_change_bits();

        for (unsigned int poll = 0; poll < 40; poll++){
            hprt = HPRT0;
            if (hprt & HPRT0_ENA){
                g_port_speed = hprt & HPRT0_SPD_MASK;
                g_ep0_mps = (g_port_speed == HPRT0_SPD_HIGH) ? 64u : USB_CTRL_EP_MPS_DEFAULT;
                uart_puts("USB: root port reset complete, speed=");
                if (g_port_speed == HPRT0_SPD_HIGH){
                    uart_puts("high\n");
                } else if (g_port_speed == HPRT0_SPD_FULL){
                    uart_puts("full\n");
                } else if (g_port_speed == HPRT0_SPD_LOW){
                    uart_puts("low\n");
                } else{
                    uart_puts("unknown\n");
                }
                return 0;
            }
            if (hprt & (HPRT0_ENA_CHG | HPRT0_CONN_DET | HPRT0_OVRCURR_CHG)){
                clear_port_change_bits();
            }
            spin_delay(250000);
        }
    }

    uart_puts("USB: root port did not enable after reset.\n");
    uart_puts("USB: HPRT0=");
    uart_puthex(HPRT0);
    uart_puts("\n");
    return -1;
}

int usb_host_init(void){
    g_usb_ready = 0;

    unsigned int id = GSNPSID;
    uart_puts("USB: GSNPSID=");
    uart_puthex(id);
    uart_puts("\n");

    if (id == 0 || id == 0xFFFFFFFFu){
        uart_puts("USB: DWC2 not responding.\n");
        return -1;
    }

    // Power/clock gate disable for the core.
    PCGCTL = 0;
    asm volatile("dsb sy");
    asm volatile("isb");

    if (wait_mask_set((volatile unsigned int*)&GRSTCTL, GRSTCTL_AHB_IDLE, 2000000) != 0){
        uart_puts("USB: AHB idle timeout.\n");
        return -1;
    }

    // Core soft reset.
    GRSTCTL |= GRSTCTL_CSRST;
    if (wait_mask_clear((volatile unsigned int*)&GRSTCTL, GRSTCTL_CSRST, 2000000) != 0){
        uart_puts("USB: core reset timeout.\n");
        return -1;
    }
    spin_delay(50000);

    // Force host mode.
    unsigned int gusbcfg = GUSBCFG;
    gusbcfg &= ~GUSBCFG_FDMOD;
    gusbcfg |= GUSBCFG_FHMOD;
    GUSBCFG = gusbcfg;
    asm volatile("dsb sy");
    asm volatile("isb");
    spin_delay(200000);
    if (wait_host_mode(4000000) != 0){
        uart_puts("USB: failed to enter host mode.\n");
        uart_puts("USB: GINTSTS=");
        uart_puthex(GINTSTS);
        uart_puts(" GUSBCFG=");
        uart_puthex(GUSBCFG);
        uart_puts("\n");
        return -1;
    }

    // Basic FIFO defaults suitable for initial control transfer work.
    GRXFSIZ = 512;
    GNPTXFSIZ = (256u << 16) | 512u; // depth | start addr

    // Clear and mask interrupts for phase 1 polling path.
    GINTSTS = 0xFFFFFFFFu;
    GINTMSK = 0;
    GAHBCFG |= GAHBCFG_GLBL_INTR_EN;

    // Set full-speed PHY clock (safe default on many Pi bare-metal bring-ups).
    HCFG = 0x00000003u;
    (void)HFIR;

    // Enable port power, preserving write-1-to-clear bits.
    unsigned int hprt = HPRT0;
    hprt &= ~(HPRT0_CONN_DET | HPRT0_ENA_CHG | HPRT0_OVRCURR_CHG);
    hprt |= HPRT0_PWR;
    hprt &= ~HPRT0_RESET;
    HPRT0 = hprt;
    asm volatile("dsb sy");
    spin_delay(500000);

    g_usb_ready = 1;
    uart_puts("USB: host phase1 init OK.\n");
    usb_host_dump_state();

    if (wait_port_connect(8000000) == 0){
        if (usb_host_reset_root_port() != 0){
            uart_puts("USB: root-port reset failed during init.\n");
        }
    } else{
        uart_puts("USB: no root-port connect yet.\n");
    }
    return 0;
}

int usb_host_ready(void){
    return g_usb_ready;
}

void usb_host_dump_state(void){
    uart_puts("USB: HPRT0=");
    uart_puthex(HPRT0);
    uart_puts(" HCFG=");
    uart_puthex(HCFG);
    uart_puts(" GINTSTS=");
    uart_puthex(GINTSTS);
    uart_puts(" PCGCTL=");
    uart_puthex(PCGCTL);
    uart_puts("\n");
    if (HPRT0 & HPRT0_CONN_STS){
        uart_puts("USB: port device connected\n");
    } else{
        uart_puts("USB: no device on root port\n");
    }
    if (HPRT0 & HPRT0_ENA){
        uart_puts("USB: port enabled\n");
    }
    if (HPRT0 & HPRT0_OVRCURR){
        uart_puts("USB: overcurrent flagged\n");
    }
}

int usb_host_control_transfer(unsigned char dev_addr,
                              const usb_setup_packet_t* setup,
                              unsigned char* data,
                              unsigned int data_len,
                              int in_transfer){
    if (!g_usb_ready || !setup){
        return -1;
    }
    if (!(HPRT0 & HPRT0_ENA)){
        uart_puts("USB: control xfer while port disabled\n");
        return -1;
    }

    unsigned char setup_bytes[8];
    setup_bytes[0] = setup->bmRequestType;
    setup_bytes[1] = setup->bRequest;
    setup_bytes[2] = (unsigned char)(setup->wValue & 0xFFu);
    setup_bytes[3] = (unsigned char)((setup->wValue >> 8) & 0xFFu);
    setup_bytes[4] = (unsigned char)(setup->wIndex & 0xFFu);
    setup_bytes[5] = (unsigned char)((setup->wIndex >> 8) & 0xFFu);
    setup_bytes[6] = (unsigned char)(setup->wLength & 0xFFu);
    setup_bytes[7] = (unsigned char)((setup->wLength >> 8) & 0xFFu);

    if (hc_transfer(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_SETUP, setup_bytes, sizeof(setup_bytes), 0, 0) != 0){
        uart_puts("USB: SETUP stage failed\n");
        return -1;
    }

    if (data_len > 0){
        if (in_transfer){
            if (hc_transfer(0, dev_addr, 1, g_ep0_mps, HCTSIZ_PID_DATA1, 0, 0, data, data_len) != 0){
                uart_puts("USB: DATA IN stage failed\n");
                return -1;
            }
        } else{
            if (hc_transfer(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_DATA1, data, data_len, 0, 0) != 0){
                uart_puts("USB: DATA OUT stage failed\n");
                return -1;
            }
        }
    }

    // Status stage: opposite direction, zero-length DATA1.
    if (hc_transfer(0, dev_addr, in_transfer ? 0 : 1, g_ep0_mps, HCTSIZ_PID_DATA1, 0, 0, 0, 0) != 0){
        uart_puts("USB: STATUS stage failed\n");
        return -1;
    }

    return 0;
}

int usb_host_read_device_descriptor(unsigned char* out18, unsigned int len){
    if (!out18 || len < 18){
        return -1;
    }
    if (!g_usb_ready){
        return -1;
    }
    if (!(HPRT0 & HPRT0_CONN_STS)){
        if (wait_port_connect(6000000) == 0){
            if (usb_host_reset_root_port() != 0){
                return -1;
            }
        }
    }
    if (!(HPRT0 & HPRT0_CONN_STS)){
        uart_puts("USB: no device present for descriptor read\n");
        return -1;
    }
    if (!(HPRT0 & HPRT0_ENA)){
        if (usb_host_reset_root_port() != 0){
            uart_puts("USB: descriptor read blocked; port not enabled\n");
            return -1;
        }
    }

    usb_setup_packet_t req;
    req.bmRequestType = 0x80; // device-to-host, standard, device
    req.bRequest = 0x06;      // GET_DESCRIPTOR
    req.wValue = 0x0100;      // DEVICE descriptor, index 0
    req.wIndex = 0x0000;
    req.wLength = 8;

    for (unsigned int i = 0; i < 18; i++){
        out18[i] = 0;
    }

    unsigned int saved_mps = g_ep0_mps;
    g_ep0_mps = USB_CTRL_EP_MPS_DEFAULT;
    if (usb_host_control_transfer(0, &req, out18, 8, 1) != 0){
        g_ep0_mps = saved_mps;
        return -1;
    }

    if (out18[7] != 0){
        g_ep0_mps = out18[7];
    } else if (saved_mps != 0){
        g_ep0_mps = saved_mps;
    }

    // Re-read full descriptor now that EP0 MPS is known.
    req.wLength = 18;
    if (usb_host_control_transfer(0, &req, out18, 18, 1) != 0){
        return -1;
    }

    // Update EP0 max packet size from descriptor byte 7 for future transfers.
    if (out18[0] == 18 && out18[1] == 1 && out18[7] != 0){
        g_ep0_mps = out18[7];
    }
    return 0;
}
