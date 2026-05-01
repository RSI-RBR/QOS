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
#define GNPTXSTS  (*(volatile unsigned int*)(USB_DWC2_BASE + 0x02C))
#define GCCFG     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x038))
#define GSNPSID   (*(volatile unsigned int*)(USB_DWC2_BASE + 0x040))
#define HCFG      (*(volatile unsigned int*)(USB_DWC2_BASE + 0x400))
#define HFIR      (*(volatile unsigned int*)(USB_DWC2_BASE + 0x404))
#define HFNUM     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x408))
#define HAINT     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x414))
#define HAINTMSK  (*(volatile unsigned int*)(USB_DWC2_BASE + 0x418))
#define HPRT0     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x440))
#define PCGCTL    (*(volatile unsigned int*)(USB_DWC2_BASE + 0xE00))
#define FIFO0     (*(volatile unsigned int*)(USB_DWC2_BASE + 0x1000))

#define HC_REG_BASE(ch)   (USB_DWC2_BASE + 0x500 + ((ch) * 0x20))
#define HCCHAR(ch)        (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x00))
#define HCSPLT(ch)        (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x04))
#define HCINT(ch)         (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x08))
#define HCINTMSK(ch)      (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x0C))
#define HCTSIZ(ch)        (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x10))

// GRSTCTL bits
#define GRSTCTL_CSRST        (1u << 0)
#define GRSTCTL_AHB_IDLE     (1u << 31)
#define GRSTCTL_RXFFLSH      (1u << 4)
#define GRSTCTL_TXFFLSH      (1u << 5)
#define GRSTCTL_TXFNUM_SHIFT 6

// GUSBCFG bits
#define GUSBCFG_FHMOD        (1u << 29)
#define GUSBCFG_FDMOD        (1u << 30)

// GAHBCFG bits
#define GAHBCFG_GLBL_INTR_EN (1u << 0)
#define GAHBCFG_DMA_EN       (1u << 5)

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

// HCFG bits
#define HCFG_FSLSPCLKSEL_MASK      0x3u
#define HCFG_FSLSPCLKSEL_30_60_MHZ 0u
#define HCFG_FSLSPCLKSEL_48_MHZ    1u
// GINTSTS bits (subset)
#define GINTSTS_CURMODE_HOST (1u << 0)
#define GINTSTS_RXFLVL       (1u << 4)
#define GINTSTS_HCHINT       (1u << 25)

// Host channel bits
#define HCCHAR_MPS_MASK      0x7FFu
#define HCCHAR_EPNUM_SHIFT   11
#define HCCHAR_EPDIR         (1u << 15)
#define HCCHAR_LSPDDEV       (1u << 17)
#define HCCHAR_EPTYPE_SHIFT  18
#define HCCHAR_MULTICNT_SHIFT 20
#define HCCHAR_DEVADDR_SHIFT 22
#define HCCHAR_ODDFRM        (1u << 29)
#define HCCHAR_CHDIS         (1u << 30)
#define HCCHAR_CHENA         (1u << 31)

// HCSPLT bits
#define HCSPLT_SPLTENA       (1u << 31)
#define HCSPLT_COMPSPLT      (1u << 16)
#define HCSPLT_XACTPOS_SHIFT 14
#define HCSPLT_XACTPOS_ALL   3u
#define HCSPLT_HUBADDR_SHIFT 7
#define HCSPLT_PRTADDR_SHIFT 0

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

// Non-periodic Tx status
#define TXSTS_QSPCAVAIL_SHIFT 16
#define TXSTS_QSPCAVAIL_MASK  (0xFFu << TXSTS_QSPCAVAIL_SHIFT)

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
#define USB_HUB_DESC_TYPE 0x29u

// USB 2.0 Hub class requests/features.
#define HUB_REQ_GET_STATUS      0x00u
#define HUB_REQ_CLEAR_FEATURE   0x01u
#define HUB_REQ_SET_FEATURE     0x03u
#define HUB_REQ_GET_DESCRIPTOR  0x06u
#define HUB_REQ_SET_CONFIGURATION 0x09u

#define HUB_FEAT_PORT_RESET       4u
#define HUB_FEAT_PORT_POWER       8u
#define HUB_FEAT_C_PORT_CONNECTION 16u
#define HUB_FEAT_C_PORT_ENABLE     17u
#define HUB_FEAT_C_PORT_SUSPEND    18u
#define HUB_FEAT_C_PORT_OVER_CURRENT 19u
#define HUB_FEAT_C_PORT_RESET      20u

#define HUB_PORT_STAT_CONNECTION (1u << 0)
#define HUB_PORT_STAT_ENABLE     (1u << 1)
#define HUB_PORT_STAT_LOW_SPEED  (1u << 9)
#define HUB_PORT_STAT_HIGH_SPEED (1u << 10)

static int g_usb_ready = 0;
static unsigned int g_port_speed = HPRT0_SPD_FULL;
static unsigned int g_ep0_mps = USB_CTRL_EP_MPS_DEFAULT;
static usb_root_device_info_t g_root_info;
static int g_split_ctx_active = 0;
static int g_split_ctx_use_split = 0;
static int g_split_ctx_low_speed = 0;
static unsigned char g_split_ctx_hub_addr = 0;
static unsigned char g_split_ctx_hub_port = 0;
static int g_child_use_split = 0;
static int g_child_low_speed = 0;
static unsigned char g_status_dummy[4];
static unsigned int g_preidle_fail_logs = 0;

static int usb_std_request(unsigned char dev_addr,
                           unsigned char bmRequestType,
                           unsigned char bRequest,
                           unsigned short wValue,
                           unsigned short wIndex,
                           unsigned char* data,
                           unsigned short wLength);
static void spin_delay(unsigned int n);
static int usb_get_device_descriptor_at(unsigned char dev_addr, unsigned char* out18, unsigned int len);
static int usb_get_config_descriptor(unsigned char dev_addr,
                                     unsigned char* buf,
                                     unsigned int cap,
                                     unsigned short* total_len_out);
static int usb_get_split_route(unsigned char dev_addr, unsigned char* hub_addr, unsigned char* hub_port);
static int usb_target_is_low_speed(unsigned char dev_addr);

static unsigned short le16(const unsigned char* p){
    return (unsigned short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
}

static void usb_set_split_context(unsigned char hub_addr, unsigned char hub_port, int use_split, int low_speed){
    g_split_ctx_active = 1;
    g_split_ctx_use_split = use_split ? 1 : 0;
    g_split_ctx_low_speed = low_speed ? 1 : 0;
    g_split_ctx_hub_addr = hub_addr;
    g_split_ctx_hub_port = hub_port;
}

static void usb_clear_split_context(void){
    g_split_ctx_active = 0;
    g_split_ctx_use_split = 0;
    g_split_ctx_low_speed = 0;
    g_split_ctx_hub_addr = 0;
    g_split_ctx_hub_port = 0;
}

static int usb_get_split_route(unsigned char dev_addr, unsigned char* hub_addr, unsigned char* hub_port){
    if (!hub_addr || !hub_port){
        return 0;
    }
    if (g_port_speed != HPRT0_SPD_HIGH){
        return 0;
    }

    // Temporary route during downstream child enumeration (dev_addr may be 0).
    if (g_split_ctx_active &&
        g_split_ctx_use_split &&
        dev_addr != g_split_ctx_hub_addr){
        *hub_addr = g_split_ctx_hub_addr;
        *hub_port = g_split_ctx_hub_port;
        return 1;
    }

    // Persistent route for already-enumerated hub child (e.g. SMSC95xx control).
    if (g_root_info.child_present &&
        g_child_use_split &&
        g_root_info.child_hub_address != 0 &&
        g_root_info.child_hub_port != 0 &&
        dev_addr == g_root_info.child_address){
        *hub_addr = g_root_info.child_hub_address;
        *hub_port = g_root_info.child_hub_port;
        return 1;
    }

    return 0;
}

static int usb_target_is_low_speed(unsigned char dev_addr){
    if (g_port_speed == HPRT0_SPD_LOW){
        return 1;
    }

    if (g_split_ctx_active &&
        g_split_ctx_use_split &&
        g_split_ctx_low_speed &&
        dev_addr != g_split_ctx_hub_addr){
        return 1;
    }

    if (g_root_info.child_present &&
        g_child_use_split &&
        g_child_low_speed &&
        dev_addr == g_root_info.child_address){
        return 1;
    }

    return 0;
}

static int hub_port_set_feature(unsigned char hub_addr, unsigned short port, unsigned short feat){
    return usb_std_request(hub_addr, 0x23, HUB_REQ_SET_FEATURE, feat, port, 0, 0);
}

static int hub_port_clear_feature(unsigned char hub_addr, unsigned short port, unsigned short feat){
    return usb_std_request(hub_addr, 0x23, HUB_REQ_CLEAR_FEATURE, feat, port, 0, 0);
}

static int hub_port_get_status(unsigned char hub_addr, unsigned short port, unsigned short* stat, unsigned short* change){
    unsigned char st[4];
    if (usb_std_request(hub_addr, 0xA3, HUB_REQ_GET_STATUS, 0, port, st, sizeof(st)) != 0){
        return -1;
    }
    if (stat){
        *stat = le16(&st[0]);
    }
    if (change){
        *change = le16(&st[2]);
    }
    return 0;
}

static void hub_port_clear_change_bits(unsigned char hub_addr, unsigned short port){
    (void)hub_port_clear_feature(hub_addr, port, HUB_FEAT_C_PORT_CONNECTION);
    (void)hub_port_clear_feature(hub_addr, port, HUB_FEAT_C_PORT_ENABLE);
    (void)hub_port_clear_feature(hub_addr, port, HUB_FEAT_C_PORT_SUSPEND);
    (void)hub_port_clear_feature(hub_addr, port, HUB_FEAT_C_PORT_OVER_CURRENT);
    (void)hub_port_clear_feature(hub_addr, port, HUB_FEAT_C_PORT_RESET);
}

static int usb_enumerate_hub_downstream_child(unsigned char hub_addr, unsigned short port, unsigned char child_addr){
    unsigned short st = 0;
    unsigned short chg = 0;
    unsigned char dev_desc[18];
    unsigned char cfg_desc[256];
    unsigned short cfg_total = 0;
    int child_is_high_speed = 0;
    int child_is_low_speed = 0;
    int child_use_split = 0;

    if (hub_port_set_feature(hub_addr, port, HUB_FEAT_PORT_POWER) != 0){
        uart_puts("USB: hub port power failed\n");
        return -1;
    }
    spin_delay(1000000);

    if (hub_port_get_status(hub_addr, port, &st, &chg) != 0){
        uart_puts("USB: hub port status read failed\n");
        return -1;
    }
    if (!(st & HUB_PORT_STAT_CONNECTION)){
        uart_puts("USB: hub child not connected on port ");
        uart_puthex(port);
        uart_puts("\n");
        return -1;
    }

    if (hub_port_set_feature(hub_addr, port, HUB_FEAT_PORT_RESET) != 0){
        uart_puts("USB: hub port reset set failed\n");
        return -1;
    }
    spin_delay(50000000);

    hub_port_clear_change_bits(hub_addr, port);

    for (unsigned int i = 0; i < 40; i++){
        if (hub_port_get_status(hub_addr, port, &st, &chg) == 0){
            if ((st & HUB_PORT_STAT_CONNECTION) && (st & HUB_PORT_STAT_ENABLE)){
                break;
            }
        }
        spin_delay(400000);
        if (i == 39){
            uart_puts("USB: hub child port failed to enable\n");
            return -1;
        }
    }

    child_is_high_speed = (st & HUB_PORT_STAT_HIGH_SPEED) ? 1 : 0;
    child_is_low_speed = (st & HUB_PORT_STAT_LOW_SPEED) ? 1 : 0;
    child_use_split = child_is_high_speed ? 0 : 1;

    usb_set_split_context(hub_addr, (unsigned char)port, child_use_split, child_is_low_speed);

    uart_puts("USB: hub child speed=");
    if (child_is_high_speed){
        uart_puts("high");
    } else if (child_is_low_speed){
        uart_puts("low");
    } else{
        uart_puts("full");
    }
    uart_puts(child_use_split ? " split\n" : " direct\n");

    g_ep0_mps = USB_CTRL_EP_MPS_DEFAULT;
    if (usb_get_device_descriptor_at(0, dev_desc, sizeof(dev_desc)) != 0){
        uart_puts("USB: hub child desc@0 failed\n");
        usb_clear_split_context();
        return -1;
    }
    if (usb_std_request(0, 0x00, 0x05, child_addr, 0, 0, 0) != 0){
        uart_puts("USB: hub child SET_ADDRESS failed\n");
        usb_clear_split_context();
        return -1;
    }
    spin_delay(300000);

    if (usb_get_device_descriptor_at(child_addr, dev_desc, sizeof(dev_desc)) != 0){
        uart_puts("USB: hub child desc@addr failed\n");
        usb_clear_split_context();
        return -1;
    }

    g_root_info.child_present = 1;
    g_root_info.child_address = child_addr;
    g_root_info.child_class = dev_desc[4];
    g_root_info.child_vid = le16(&dev_desc[8]);
    g_root_info.child_pid = le16(&dev_desc[10]);
    g_child_use_split = child_use_split;
    g_child_low_speed = child_is_low_speed;
    if (child_use_split){
        g_root_info.child_hub_address = hub_addr;
        g_root_info.child_hub_port = (unsigned char)port;
    } else{
        g_root_info.child_hub_address = 0;
        g_root_info.child_hub_port = 0;
    }

    int cfg_read = usb_get_config_descriptor(child_addr, cfg_desc, sizeof(cfg_desc), &cfg_total);
    if (cfg_read >= 6){
        g_root_info.child_config_value = cfg_desc[5];
        if (g_root_info.child_config_value != 0){
            if (usb_std_request(child_addr, 0x00, HUB_REQ_SET_CONFIGURATION, g_root_info.child_config_value, 0, 0, 0) == 0){
                g_root_info.child_configured = 1;
            }
        }
    }

    uart_puts("USB: hub child addr=");
    uart_puthex(g_root_info.child_address);
    uart_puts(" vid=");
    uart_puthex(g_root_info.child_vid);
    uart_puts(" pid=");
    uart_puthex(g_root_info.child_pid);
    uart_puts(" class=");
    uart_puthex(g_root_info.child_class);
    uart_puts(" cfg=");
    uart_puthex(g_root_info.child_config_value);
    uart_puts(g_root_info.child_configured ? " (set)\n" : " (not set)\n");
    usb_clear_split_context();
    return 0;
}

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

static int hc_wait_idle(unsigned int ch, unsigned int loops){
    while (loops--){
        unsigned int hcchar = HCCHAR(ch);
        if ((hcchar & HCCHAR_CHENA) == 0){
            return 0;
        }
        if (HCINT(ch) & HCINT_CHHLTD){
            HCINT(ch) = HCINT_CHHLTD;
            if ((HCCHAR(ch) & HCCHAR_CHENA) == 0){
                return 0;
            }
        }
    }
    return -1;
}

static void usb_flush_host_fifos(void){
    // DWC2 recommends flushing FIFOs when endpoint/channel state gets stuck.
    if (wait_mask_set((volatile unsigned int*)&GRSTCTL, GRSTCTL_AHB_IDLE, 2000000) != 0){
        return;
    }

    unsigned int reset = GRSTCTL_RXFFLSH | GRSTCTL_TXFFLSH | (0x10u << GRSTCTL_TXFNUM_SHIFT);
    GRSTCTL = reset;
    (void)wait_mask_clear((volatile unsigned int*)&GRSTCTL, GRSTCTL_RXFFLSH | GRSTCTL_TXFFLSH, 2000000);
    spin_delay(1000);
}

static int hc_force_halt(unsigned int ch){
    unsigned int hcchar = HCCHAR(ch);
    if ((hcchar & HCCHAR_CHENA) == 0){
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = 0;
        HCSPLT(ch) = 0;
        return 0;
    }

    HCINT(ch) = 0xFFFFFFFFu;
    HCINTMSK(ch) = HCINT_CHHLTD | HCINT_ERROR_MASK | HCINT_NAK | HCINT_ACK | HCINT_NYET;

    // Try multiple halt-edge retriggers. Some DWC2 revisions can stick in
    // CHENA|CHDIS until CHDIS is toggled and re-issued.
    for (unsigned int attempt = 0; attempt < 12; attempt++){
        hcchar = HCCHAR(ch);
        if ((hcchar & HCCHAR_CHENA) == 0){
            HCINT(ch) = 0xFFFFFFFFu;
            HCINTMSK(ch) = 0;
            HCSPLT(ch) = 0;
            return 0;
        }

        unsigned int qspc = (GNPTXSTS & TXSTS_QSPCAVAIL_MASK) >> TXSTS_QSPCAVAIL_SHIFT;
        unsigned int base = hcchar & ~HCCHAR_EPDIR;

        // Drop CHDIS first so the next write creates a fresh halt edge.
        HCCHAR(ch) = base & ~HCCHAR_CHDIS;
        spin_delay(200);

        // If request queue has space, issue CHENA|CHDIS (normal halt request).
        // If full, issue CHDIS-only and retry.
        unsigned int halt_req = base | HCCHAR_CHDIS;
        if (qspc != 0){
            halt_req |= HCCHAR_CHENA;
        } else{
            halt_req &= ~HCCHAR_CHENA;
        }
        HCCHAR(ch) = halt_req;

        if (hc_wait_idle(ch, 300000) == 0){
            HCINT(ch) = 0xFFFFFFFFu;
            HCINTMSK(ch) = 0;
            HCSPLT(ch) = 0;
            return 0;
        }

        if ((attempt % 4u) == 3u){
            usb_flush_host_fifos();
        }
        spin_delay(5000);
    }

    // Final brute-force cleanup.
    HCSPLT(ch) = 0;
    HCTSIZ(ch) = 0;
    hcchar = HCCHAR(ch);
    hcchar &= ~(HCCHAR_CHENA | HCCHAR_CHDIS | HCCHAR_EPDIR | HCCHAR_ODDFRM);
    HCCHAR(ch) = hcchar;
    spin_delay(2000);
    if ((HCCHAR(ch) & HCCHAR_CHENA) == 0){
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = 0;
        return 0;
    }

    HCINT(ch) = 0xFFFFFFFFu;
    HCINTMSK(ch) = 0;
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
    unsigned int saw_complete = 0;

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

        if (hcint & (HCINT_NAK | HCINT_NYET)){
            HCINT(ch) = (hcint & (HCINT_NAK | HCINT_NYET));
            return 1;
        }

        // Keep a completion breadcrumb, but do not return success until
        // channel-halt is observed so next stage sees a clean channel state.
        if (hcint & HCINT_XFERCOMPL){
            HCINT(ch) = HCINT_XFERCOMPL;
            saw_complete = 1;
            hcint &= ~HCINT_XFERCOMPL;
        }
        if (hcint & HCINT_ACK){
            HCINT(ch) = HCINT_ACK;
            saw_complete = 1;
            hcint &= ~HCINT_ACK;
        }

        // Some DWC2 variants complete control stages without a reliable
        // CHHLTD edge in polling mode. If completion was observed and the
        // channel is now disabled, treat it as success.
        if (saw_complete && ((HCCHAR(ch) & HCCHAR_CHENA) == 0)){
            return 0;
        }

        if (hcint & HCINT_CHHLTD){
            HCINT(ch) = HCINT_CHHLTD;
            return saw_complete ? 0 : 1;
        }
    }
    return -1;
}

static int hc_transfer_reg(unsigned int ch,
                           unsigned char dev_addr,
                           int ep_in,
                           unsigned int ep_mps,
                           unsigned int pid,
                           const unsigned char* out_data,
                           unsigned int out_len,
                           unsigned char* in_data,
                           unsigned int in_len,
                           unsigned int hcsplt_reg){
    unsigned int xfer_len = ep_in ? in_len : out_len;
    unsigned int pktcnt = (xfer_len == 0) ? 1u : div_round_up(xfer_len, ep_mps ? ep_mps : 1u);
    if (pktcnt == 0){
        pktcnt = 1;
    }

    // Ensure channel is idle before programming a new transfer.
    if (hc_wait_idle(ch, 500000) != 0){
        if (hc_force_halt(ch) != 0){
            if (g_preidle_fail_logs < 4 || (g_preidle_fail_logs & 0x3Fu) == 0u){
                uart_puts("USB: HC not idle before xfer\n");
                uart_puts("USB: pre-idle hcchar=");
                uart_puthex(HCCHAR(ch));
                uart_puts(" hctsiz=");
                uart_puthex(HCTSIZ(ch));
                uart_puts(" hcint=");
                uart_puthex(HCINT(ch));
                uart_puts("\n");
            }
            g_preidle_fail_logs++;
            return -1;
        }
        g_preidle_fail_logs = 0;
    }

    for (unsigned int attempt = 0; attempt < 16; attempt++){
        if (attempt > 0){
            if (hc_force_halt(ch) != 0){
                return -1;
            }
        }
        HCINT(ch) = 0xFFFFFFFFu;
        HAINTMSK |= (1u << ch);
        GINTMSK |= GINTSTS_HCHINT;
        HCINTMSK(ch) = HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_ERROR_MASK | HCINT_NAK | HCINT_ACK | HCINT_NYET;
        HCSPLT(ch) = hcsplt_reg;

        unsigned int hctsiz = (xfer_len & HCTSIZ_XFERSIZE_MASK)
            | (pktcnt << HCTSIZ_PKTCNT_SHIFT)
            | ((pid & 0x3u) << HCTSIZ_PID_SHIFT);
        HCTSIZ(ch) = hctsiz;

        unsigned int hcchar = (ep_mps & HCCHAR_MPS_MASK)
            | ((0u & 0xFu) << HCCHAR_EPNUM_SHIFT)
            | ((dev_addr & 0x7Fu) << HCCHAR_DEVADDR_SHIFT)
            | (0u << HCCHAR_EPTYPE_SHIFT)   // control
            | (1u << HCCHAR_MULTICNT_SHIFT);
        if (ep_in){
            hcchar |= HCCHAR_EPDIR;
        }
        if (usb_target_is_low_speed(dev_addr)){
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
            g_preidle_fail_logs = 0;
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
            uart_puts(" hcsplt=");
            uart_puthex(HCSPLT(ch));
            uart_puts("\n");
            (void)hc_force_halt(ch);
            return -1;
        }
        // Transient NAK/NYET/halt: short settle then retry.
        if (hc_force_halt(ch) != 0){
            return -1;
        }
        spin_delay(90000);
    }

    uart_puts("USB: hc xfer retry exhausted\n");
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
    return hc_transfer_reg(ch, dev_addr, ep_in, ep_mps, pid, out_data, out_len, in_data, in_len, 0u);
}

static int hc_transfer_split(unsigned int ch,
                             unsigned char dev_addr,
                             int ep_in,
                             unsigned int ep_mps,
                             unsigned int pid,
                             const unsigned char* out_data,
                             unsigned int out_len,
                             unsigned char* in_data,
                             unsigned int in_len,
                             unsigned char hub_addr,
                             unsigned char hub_port){
    unsigned int split_reg = HCSPLT_SPLTENA
        | ((HCSPLT_XACTPOS_ALL & 0x3u) << HCSPLT_XACTPOS_SHIFT)
        | (((unsigned int)hub_addr & 0x7Fu) << HCSPLT_HUBADDR_SHIFT)
        | (((unsigned int)hub_port & 0x7Fu) << HCSPLT_PRTADDR_SHIFT);

    // Align split scheduling to microframe boundary for better stability.
    for (unsigned int i = 0; i < 2000000; i++){
        if ((HFNUM & 0x7u) == 0u){
            break;
        }
        if (i == 1999999){
            uart_puts("USB: split frame wait timeout\n");
        }
    }

    if (ep_in){
        if (hc_transfer_reg(ch, dev_addr, 1, ep_mps, pid, 0, 0, 0, 0, split_reg) != 0){
            HCSPLT(ch) = 0;
            return -1;
        }
        if (hc_transfer_reg(ch, dev_addr, 1, ep_mps, pid, 0, 0, in_data, in_len, split_reg | HCSPLT_COMPSPLT) != 0){
            HCSPLT(ch) = 0;
            return -1;
        }
    } else{
        if (hc_transfer_reg(ch, dev_addr, 0, ep_mps, pid, out_data, out_len, 0, 0, split_reg) != 0){
            HCSPLT(ch) = 0;
            return -1;
        }
        if (hc_transfer_reg(ch, dev_addr, 0, ep_mps, pid, g_status_dummy, 0, 0, 0, split_reg | HCSPLT_COMPSPLT) != 0){
            HCSPLT(ch) = 0;
            return -1;
        }
    }

    HCSPLT(ch) = 0;
    return 0;
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
    for (unsigned int i = 0; i < sizeof(g_root_info); i++){
        ((unsigned char*)&g_root_info)[i] = 0;
    }
    g_child_use_split = 0;
    g_child_low_speed = 0;

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
    GINTMSK = GINTSTS_HCHINT | GINTSTS_RXFLVL;
    HAINTMSK = 0xFFFFFFFFu;
    // Force slave mode path: our host transfer code uses FIFO IO (no HCDMA).
    GAHBCFG &= ~GAHBCFG_DMA_EN;
    GAHBCFG |= GAHBCFG_GLBL_INTR_EN;

    // Match the common DWC2 host setup path used by Linux/U-Boot on BCM SoCs.
    HCFG = (HCFG & ~HCFG_FSLSPCLKSEL_MASK) | HCFG_FSLSPCLKSEL_48_MHZ;
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
        } else{
            // Start from a clean FIFO state before first enumeration transfer.
            usb_flush_host_fifos();
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
    uart_puts(" GAHBCFG=");
    uart_puthex(GAHBCFG);
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

    unsigned char split_hub_addr = 0;
    unsigned char split_hub_port = 0;
    int use_split = usb_get_split_route(dev_addr, &split_hub_addr, &split_hub_port);
    const unsigned int max_attempts = 1;

    // If a previous transfer left CH0 wedged, recover once before issuing a new setup.
    if (hc_wait_idle(0, 200000) != 0){
        (void)hc_force_halt(0);
        HCSPLT(0) = 0;
        usb_flush_host_fifos();
    }

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++){
        int use_split_runtime = use_split;
        int failed_stage = 0; // 1=SETUP, 2=DATA, 3=STATUS

        if (use_split_runtime){
            if (hc_transfer_split(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_SETUP,
                                  setup_bytes, sizeof(setup_bytes), 0, 0,
                                  split_hub_addr, split_hub_port) != 0){
                // Some children (e.g. HS functions behind LAN9514) must use direct
                // transactions even though they're downstream of a HS hub.
                if (hc_transfer(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_SETUP,
                                setup_bytes, sizeof(setup_bytes), 0, 0) == 0){
                    uart_puts("USB: split setup failed, fallback direct\n");
                    use_split_runtime = 0;
                } else{
                    failed_stage = 1;
                }
            }
        } else if (hc_transfer(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_SETUP, setup_bytes, sizeof(setup_bytes), 0, 0) != 0){
            failed_stage = 1;
        }

        if (failed_stage == 0 && data_len > 0){
            if (in_transfer){
                int rc = use_split_runtime
                    ? hc_transfer_split(0, dev_addr, 1, g_ep0_mps, HCTSIZ_PID_DATA1,
                                        0, 0, data, data_len, split_hub_addr, split_hub_port)
                    : hc_transfer(0, dev_addr, 1, g_ep0_mps, HCTSIZ_PID_DATA1, 0, 0, data, data_len);
                if (rc != 0){
                    failed_stage = 2;
                }
            } else{
                int rc = use_split_runtime
                    ? hc_transfer_split(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_DATA1,
                                        data, data_len, 0, 0, split_hub_addr, split_hub_port)
                    : hc_transfer(0, dev_addr, 0, g_ep0_mps, HCTSIZ_PID_DATA1, data, data_len, 0, 0);
                if (rc != 0){
                    failed_stage = 2;
                }
            }
        }

        if (failed_stage == 0){
            // Status stage: opposite direction, zero-length DATA1.
            int status_rc = use_split_runtime
                ? hc_transfer_split(0, dev_addr, in_transfer ? 0 : 1, g_ep0_mps, HCTSIZ_PID_DATA1,
                                    0, 0, 0, 0, split_hub_addr, split_hub_port)
                : hc_transfer(0, dev_addr, in_transfer ? 0 : 1, g_ep0_mps, HCTSIZ_PID_DATA1, 0, 0, 0, 0);
            if (status_rc != 0){
                failed_stage = 3;
            }
        }

        if (failed_stage == 0){
            return 0;
        }

        if (attempt + 1u >= max_attempts){
            if (failed_stage == 1){
                uart_puts(use_split_runtime ? "USB: SETUP stage failed (split)\n" : "USB: SETUP stage failed\n");
            } else if (failed_stage == 2){
                uart_puts(in_transfer ? "USB: DATA IN stage failed\n" : "USB: DATA OUT stage failed\n");
            } else{
                uart_puts("USB: STATUS stage failed\n");
            }
            return -1;
        }

        // Recovery before retrying the full control transfer.
        (void)hc_force_halt(0);
        HCSPLT(0) = 0;
        HCINT(0) = 0xFFFFFFFFu;
        HCINTMSK(0) = 0;
        if (attempt & 1u){
            usb_flush_host_fifos();
        }
        spin_delay(120000);
    }

    return -1;
}

static int usb_std_request(unsigned char dev_addr,
                           unsigned char bmRequestType,
                           unsigned char bRequest,
                           unsigned short wValue,
                           unsigned short wIndex,
                           unsigned char* data,
                           unsigned short wLength){
    usb_setup_packet_t req;
    req.bmRequestType = bmRequestType;
    req.bRequest = bRequest;
    req.wValue = wValue;
    req.wIndex = wIndex;
    req.wLength = wLength;
    return usb_host_control_transfer(dev_addr, &req, data, wLength, (bmRequestType & 0x80u) ? 1 : 0);
}

static int usb_get_device_descriptor_at(unsigned char dev_addr, unsigned char* out18, unsigned int len){
    if (!out18 || len < 18){
        return -1;
    }

    for (unsigned int i = 0; i < 18; i++){
        out18[i] = 0;
    }

    // First 8 bytes to discover bMaxPacketSize0 safely.
    if (usb_std_request(dev_addr, 0x80, 0x06, 0x0100, 0x0000, out18, 8) != 0){
        return -1;
    }
    if (out18[7] != 0){
        g_ep0_mps = out18[7];
    }

    // Full descriptor with established EP0 MPS.
    if (usb_std_request(dev_addr, 0x80, 0x06, 0x0100, 0x0000, out18, 18) != 0){
        return -1;
    }
    return 0;
}

static int usb_get_config_descriptor(unsigned char dev_addr,
                                     unsigned char* buf,
                                     unsigned int cap,
                                     unsigned short* total_len_out){
    if (!buf || cap < 9){
        return -1;
    }

    for (unsigned int i = 0; i < cap; i++){
        buf[i] = 0;
    }

    if (usb_std_request(dev_addr, 0x80, 0x06, 0x0200, 0x0000, buf, 9) != 0){
        return -1;
    }
    unsigned short total = le16(&buf[2]);
    if (total < 9){
        return -1;
    }
    if (total_len_out){
        *total_len_out = total;
    }

    unsigned short want = total;
    if (want > cap){
        want = (unsigned short)cap;
    }
    if (usb_std_request(dev_addr, 0x80, 0x06, 0x0200, 0x0000, buf, want) != 0){
        return -1;
    }
    return (int)want;
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
    g_ep0_mps = USB_CTRL_EP_MPS_DEFAULT;
    return usb_get_device_descriptor_at(0, out18, len);
}

int usb_host_enumerate_root_device(void){
    unsigned char dev_desc[18];
    unsigned char cfg_desc[256];
    unsigned short cfg_total = 0;
    const unsigned char new_addr = 1;

    for (unsigned int i = 0; i < sizeof(g_root_info); i++){
        ((unsigned char*)&g_root_info)[i] = 0;
    }
    g_child_use_split = 0;
    g_child_low_speed = 0;

    if (!g_usb_ready){
        return -1;
    }
    if (!(HPRT0 & HPRT0_CONN_STS)){
        if (wait_port_connect(6000000) != 0){
            uart_puts("USB: enumerate: no device connected\n");
            return -1;
        }
    }
    if (!(HPRT0 & HPRT0_ENA)){
        if (usb_host_reset_root_port() != 0){
            uart_puts("USB: enumerate: port enable failed\n");
            return -1;
        }
    }

    g_ep0_mps = USB_CTRL_EP_MPS_DEFAULT;
    if (usb_get_device_descriptor_at(0, dev_desc, sizeof(dev_desc)) != 0){
        uart_puts("USB: enumerate: dev desc@0 failed\n");
        return -1;
    }

    if (usb_std_request(0, 0x00, 0x05, new_addr, 0, 0, 0) != 0){
        uart_puts("USB: enumerate: SET_ADDRESS failed\n");
        return -1;
    }
    // USB2 spec: up to 2ms recovery after status stage.
    spin_delay(300000);

    if (usb_get_device_descriptor_at(new_addr, dev_desc, sizeof(dev_desc)) != 0){
        uart_puts("USB: enumerate: dev desc@addr failed\n");
        return -1;
    }

    g_root_info.present = 1;
    g_root_info.address = new_addr;
    g_root_info.ep0_mps = dev_desc[7];
    g_root_info.dev_class = dev_desc[4];
    g_root_info.dev_subclass = dev_desc[5];
    g_root_info.dev_protocol = dev_desc[6];
    g_root_info.vid = le16(&dev_desc[8]);
    g_root_info.pid = le16(&dev_desc[10]);

    int cfg_read = usb_get_config_descriptor(new_addr, cfg_desc, sizeof(cfg_desc), &cfg_total);
    if (cfg_read < 0){
        uart_puts("USB: enumerate: config desc read failed\n");
        return -1;
    }
    g_root_info.config_total_len = cfg_total;
    if ((unsigned int)cfg_read >= 6){
        g_root_info.config_value = cfg_desc[5];
    }

    if (g_root_info.config_value != 0){
        if (usb_std_request(new_addr, 0x00, 0x09, g_root_info.config_value, 0, 0, 0) == 0){
            g_root_info.configured = 1;
        } else{
            uart_puts("USB: enumerate: SET_CONFIGURATION failed\n");
        }
    }

    uart_puts("USB: root dev addr=");
    uart_puthex(g_root_info.address);
    uart_puts(" vid=");
    uart_puthex(g_root_info.vid);
    uart_puts(" pid=");
    uart_puthex(g_root_info.pid);
    uart_puts(" class=");
    uart_puthex(g_root_info.dev_class);
    uart_puts(" cfg=");
    uart_puthex(g_root_info.config_value);
    uart_puts("\n");

    // Raspberry Pi 3 onboard path: root device is usually LAN9514 hub.
    if (g_root_info.vid == 0x0424 && g_root_info.pid == 0x9514 && g_root_info.dev_class == 0x09){
        unsigned char hub_desc[9];
        for (unsigned int i = 0; i < sizeof(hub_desc); i++){
            hub_desc[i] = 0;
        }
        if (usb_std_request(g_root_info.address, 0xA0, HUB_REQ_GET_DESCRIPTOR,
                            (unsigned short)(USB_HUB_DESC_TYPE << 8), 0, hub_desc, sizeof(hub_desc)) == 0){
            unsigned int ports = hub_desc[2];
            unsigned int pwr_on_2ms = hub_desc[5];
            uart_puts("USB: hub ports=");
            uart_puthex(ports);
            uart_puts(" pwr2good=");
            uart_puthex(pwr_on_2ms);
            uart_puts("\n");
            if (ports > 0){
                int child_found = 0;
                unsigned char next_addr = 2;
                unsigned int settle_loops = 2000000u + (pwr_on_2ms * 800000u);

                // Power all downstream ports first, then wait once.
                for (unsigned int port = 1; port <= ports; port++){
                    (void)hub_port_set_feature(g_root_info.address, (unsigned short)port, HUB_FEAT_PORT_POWER);
                }
                spin_delay(settle_loops);

                // Dump initial downstream status snapshot for debugging.
                for (unsigned int port = 1; port <= ports; port++){
                    unsigned short st = 0, chg = 0;
                    if (hub_port_get_status(g_root_info.address, (unsigned short)port, &st, &chg) == 0){
                        uart_puts("USB: hub p");
                        uart_puthex(port);
                        uart_puts(" st=");
                        uart_puthex(st);
                        uart_puts(" ch=");
                        uart_puthex(chg);
                        uart_puts("\n");
                    }
                }

                for (unsigned int port = 1; port <= ports && next_addr < 16; port++){
                    if (usb_enumerate_hub_downstream_child(g_root_info.address, (unsigned short)port, next_addr) == 0){
                        child_found = 1;
                        next_addr++;
                        // Stop at first successful child for now; enough to reach Ethernet function.
                        break;
                    }
                }
                if (!child_found){
                    uart_puts("USB: no hub child enumerated on any downstream port\n");
                }
            }
        } else{
            uart_puts("USB: hub descriptor read failed\n");
        }
    }
    return 0;
}

int usb_host_get_root_device_info(usb_root_device_info_t* out_info){
    if (!out_info || !g_root_info.present){
        return -1;
    }
    *out_info = g_root_info;
    return 0;
}
