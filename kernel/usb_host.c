#include "usb_host.h"
#include "uart.h"

#define USBHOST_VERBOSE 0
#if USBHOST_VERBOSE == 0
#define uart_puts(...) ((void)0)
#define uart_puthex(...) ((void)0)
#define uart_putdec(...) ((void)0)
#endif

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
#define HPTXFSIZ  (*(volatile unsigned int*)(USB_DWC2_BASE + 0x100))
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
#define HCDMA(ch)         (*(volatile unsigned int*)(HC_REG_BASE(ch) + 0x14))

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
#define HCINT_ERROR_MASK     (HCINT_AHBERR | HCINT_STALL | HCINT_BBLERR | HCINT_DATATGLERR)

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
#define USB_DESC_TYPE_INTERFACE 0x04u
#define USB_DESC_TYPE_ENDPOINT 0x05u
#define USB_ENDPOINT_XFER_BULK 0x02u
#define USB_ENDPOINT_XFER_INTERRUPT 0x03u
#define USB_DMA_BUFFER_SIZE 2048u
#define USB_DWC2_CHANNELS 8u
#define GPU_UNCACHED_BASE 0xC0000000UL

#define HC_EPTYPE_CONTROL 0u
#define HC_EPTYPE_BULK    2u
#define HC_EPTYPE_INTERRUPT 3u

#define USB_HID_REQ_SET_IDLE     0x0Au
#define USB_HID_REQ_SET_PROTOCOL 0x0Bu
#define USB_HID_REPORT_LEN 8u
#define USB_HID_CHAR_QUEUE_LEN 128u
#define USB_HID_ACTIVE_POLL_MS 4u
#define USB_HID_IDLE_POLL_MS 32u
#define USB_HID_ACTIVE_HOLD_MS 250u

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
static int g_usb_dma_mode = 1;
static unsigned char g_usb_dma_buffer[USB_DMA_BUFFER_SIZE] __attribute__((aligned(64)));
static unsigned char g_bulk_in_toggle[16];
static unsigned char g_bulk_out_toggle[16];
static unsigned long g_kbd_next_poll_tick = 0;
static unsigned long g_kbd_active_until_tick = 0;
static unsigned int g_hub_ports = 0;
static unsigned int g_hub_connected_mask = 0;
static unsigned int g_hub_enum_attempts = 0;
static unsigned int g_hub_enum_success = 0;
static unsigned int g_hub_enum_success_mask = 0;
static unsigned int g_hub_hid_candidates = 0;
static unsigned int g_hub_hid_candidate_mask = 0;
static unsigned char g_port_addr[USB_HOST_MAX_TRACKED_PORTS];
static unsigned char g_port_class[USB_HOST_MAX_TRACKED_PORTS];
static unsigned char g_port_config[USB_HOST_MAX_TRACKED_PORTS];
static unsigned char g_port_intr_in_ep[USB_HOST_MAX_TRACKED_PORTS];
static unsigned short g_port_vid[USB_HOST_MAX_TRACKED_PORTS];
static unsigned short g_port_pid[USB_HOST_MAX_TRACKED_PORTS];
static unsigned short g_port_intr_in_mps[USB_HOST_MAX_TRACKED_PORTS];
extern volatile unsigned long system_ticks;

typedef struct {
    int present;
    unsigned char addr;
    unsigned char iface;
    unsigned char in_ep;
    unsigned short in_mps;
    unsigned char hub_addr;
    unsigned char hub_port;
    int use_split;
    int low_speed;
    int boot_kbd;
    unsigned char in_toggle;
    unsigned char prev_report[USB_HID_REPORT_LEN];
    int have_prev_report;
    unsigned long last_report_tick;
    unsigned char q[USB_HID_CHAR_QUEUE_LEN];
    unsigned int q_head;
    unsigned int q_tail;
    unsigned int q_count;
} usb_hid_keyboard_state_t;

static usb_hid_keyboard_state_t g_kbd;

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
static void usb_dcache_clean_invalidate_range(unsigned long start, unsigned long size);
static void usb_dcache_invalidate_range(unsigned long start, unsigned long size);
static void usb_set_split_context(unsigned char hub_addr, unsigned char hub_port, int use_split, int low_speed);
static void usb_clear_split_context(void);
static void usb_parse_bulk_endpoints_from_config(const unsigned char* cfg,
                                                 unsigned int len,
                                                 unsigned char* out_in_ep,
                                                 unsigned short* out_in_mps,
                                                 unsigned char* out_out_ep,
                                                 unsigned short* out_out_mps);
static void usb_parse_interrupt_in_endpoint_from_config(const unsigned char* cfg,
                                                       unsigned int len,
                                                       unsigned char* out_iface,
                                                       unsigned char* out_ep,
                                                       unsigned short* out_mps);
static void usb_reset_hub_diag(void);
static void usb_hid_queue_reset(void);
static int usb_hid_queue_push(unsigned char c);
static int usb_hid_queue_pop(char* out);
static int usb_hid_key_present(const unsigned char* report, unsigned char key);
static unsigned char usb_hid_keycode_to_ascii(unsigned char key, int shift);
static void usb_hid_process_report(const unsigned char report[USB_HID_REPORT_LEN]);
static int usb_parse_hid_keyboard_from_config(const unsigned char* cfg,
                                              unsigned int len,
                                              unsigned char* out_iface,
                                              unsigned char* out_ep,
                                              unsigned short* out_mps,
                                              int* out_boot_kbd);
static int usb_hid_keyboard_configure(unsigned char addr,
                                      unsigned char iface,
                                      unsigned char in_ep,
                                      unsigned short in_mps,
                                      int boot_kbd,
                                      int use_split,
                                      int low_speed,
                                      unsigned char hub_addr,
                                      unsigned char hub_port);
static int usb_hid_poll_once(void);
static int hc_transfer_interrupt_in(unsigned char dev_addr,
                                    unsigned char ep_addr,
                                    unsigned int ep_mps,
                                    unsigned char* data,
                                    unsigned int len,
                                    int use_split,
                                    int low_speed,
                                    unsigned char hub_addr,
                                    unsigned char hub_port,
                                    unsigned int* actual_out);
static void usb_snapshot_hub_diag_to_root_info(void);

static unsigned short le16(const unsigned char* p){
    return (unsigned short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
}

static void usb_reset_bulk_toggles(void){
    for (unsigned int i = 0; i < sizeof(g_bulk_in_toggle); i++){
        g_bulk_in_toggle[i] = 0;
        g_bulk_out_toggle[i] = 0;
    }
}

static void usb_reset_hub_diag(void){
    g_hub_ports = 0;
    g_hub_connected_mask = 0;
    g_hub_enum_attempts = 0;
    g_hub_enum_success = 0;
    g_hub_enum_success_mask = 0;
    g_hub_hid_candidates = 0;
    g_hub_hid_candidate_mask = 0;
    for (unsigned int i = 0; i < USB_HOST_MAX_TRACKED_PORTS; i++){
        g_port_addr[i] = 0;
        g_port_class[i] = 0;
        g_port_config[i] = 0;
        g_port_intr_in_ep[i] = 0;
        g_port_vid[i] = 0;
        g_port_pid[i] = 0;
        g_port_intr_in_mps[i] = 0;
    }
}

static void usb_hid_queue_reset(void){
    g_kbd.q_head = 0;
    g_kbd.q_tail = 0;
    g_kbd.q_count = 0;
}

static int usb_hid_queue_push(unsigned char c){
    if (g_kbd.q_count >= USB_HID_CHAR_QUEUE_LEN){
        return -1;
    }
    g_kbd.q[g_kbd.q_tail] = c;
    g_kbd.q_tail = (g_kbd.q_tail + 1u) % USB_HID_CHAR_QUEUE_LEN;
    g_kbd.q_count++;
    return 0;
}

static int usb_hid_queue_pop(char* out){
    if (!out || g_kbd.q_count == 0u){
        return 0;
    }
    *out = (char)g_kbd.q[g_kbd.q_head];
    g_kbd.q_head = (g_kbd.q_head + 1u) % USB_HID_CHAR_QUEUE_LEN;
    g_kbd.q_count--;
    return 1;
}

static int usb_hid_key_present(const unsigned char* report, unsigned char key){
    if (!report || key == 0){
        return 0;
    }
    for (unsigned int i = 2; i < USB_HID_REPORT_LEN; i++){
        if (report[i] == key){
            return 1;
        }
    }
    return 0;
}

static unsigned char usb_hid_keycode_to_ascii(unsigned char key, int shift){
    if (key >= 0x04u && key <= 0x1Du){
        unsigned char base = (unsigned char)('a' + (key - 0x04u));
        if (shift){
            base = (unsigned char)('A' + (key - 0x04u));
        }
        return base;
    }

    if (key >= 0x1Eu && key <= 0x27u){
        static const unsigned char plain[] = "1234567890";
        static const unsigned char with_shift[] = "!@#$%^&*()";
        unsigned int idx = (unsigned int)(key - 0x1Eu);
        return shift ? with_shift[idx] : plain[idx];
    }

    switch (key){
        case 0x28: return '\n';
        case 0x2A: return '\b';
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}

static void usb_hid_process_report(const unsigned char report[USB_HID_REPORT_LEN]){
    if (!report){
        return;
    }

    int shift = (report[0] & 0x22u) ? 1 : 0;
    for (unsigned int i = 2; i < USB_HID_REPORT_LEN; i++){
        unsigned char key = report[i];
        if (key == 0u){
            continue;
        }
        if (key == 0x01u){
            continue;
        }
        if (g_kbd.have_prev_report && usb_hid_key_present(g_kbd.prev_report, key)){
            continue;
        }
        unsigned char ascii = usb_hid_keycode_to_ascii(key, shift);
        if (ascii){
            (void)usb_hid_queue_push(ascii);
        }
    }

    for (unsigned int i = 0; i < USB_HID_REPORT_LEN; i++){
        g_kbd.prev_report[i] = report[i];
    }
    g_kbd.have_prev_report = 1;
    g_kbd.last_report_tick = system_ticks;
}

static int usb_parse_hid_keyboard_from_config(const unsigned char* cfg,
                                              unsigned int len,
                                              unsigned char* out_iface,
                                              unsigned char* out_ep,
                                              unsigned short* out_mps,
                                              int* out_boot_kbd){
    unsigned char cur_iface = 0xFFu;
    int iface_is_hid = 0;
    int iface_is_boot_kbd = 0;
    int best_score = -1;
    unsigned char best_iface = 0u;
    unsigned char best_ep = 0u;
    unsigned short best_mps = 0u;
    int best_boot = 0;

    if (!cfg || !out_iface || !out_ep || !out_mps || !out_boot_kbd || len < 9u){
        return -1;
    }

    *out_iface = 0u;
    *out_ep = 0u;
    *out_mps = 0u;
    *out_boot_kbd = 0;

    unsigned int off = 0;
    while (off + 2u <= len){
        unsigned int desc_len = cfg[off];
        unsigned int desc_type = cfg[off + 1u];
        if (desc_len < 2u || off + desc_len > len){
            break;
        }

        if (desc_type == USB_DESC_TYPE_INTERFACE && desc_len >= 9u){
            cur_iface = cfg[off + 2u];
            unsigned char cls = cfg[off + 5u];
            unsigned char sub = cfg[off + 6u];
            unsigned char proto = cfg[off + 7u];
            iface_is_hid = (cls == 0x03u) ? 1 : 0;
            iface_is_boot_kbd = (cls == 0x03u && sub == 0x01u && proto == 0x01u) ? 1 : 0;
        } else if (iface_is_hid && desc_type == USB_DESC_TYPE_ENDPOINT && desc_len >= 7u){
            unsigned char ep_addr = cfg[off + 2u];
            unsigned char attrs = cfg[off + 3u];
            unsigned short mps = (unsigned short)(le16(&cfg[off + 4u]) & 0x7FFu);
            if ((attrs & 0x3u) == USB_ENDPOINT_XFER_INTERRUPT &&
                (ep_addr & 0x80u) &&
                mps >= 3u){
                int score = iface_is_boot_kbd ? 2 : 1;
                if (score > best_score){
                    best_score = score;
                    best_iface = cur_iface;
                    best_ep = ep_addr;
                    best_mps = mps;
                    best_boot = iface_is_boot_kbd ? 1 : 0;
                }
            }
        }

        off += desc_len;
    }

    if (best_score >= 0){
        *out_iface = best_iface;
        *out_ep = best_ep;
        *out_mps = best_mps;
        *out_boot_kbd = best_boot;
        return 0;
    }

    return -1;
}

static int usb_hid_keyboard_configure(unsigned char addr,
                                      unsigned char iface,
                                      unsigned char in_ep,
                                      unsigned short in_mps,
                                      int boot_kbd,
                                      int use_split,
                                      int low_speed,
                                      unsigned char hub_addr,
                                      unsigned char hub_port){
    if (use_split){
        usb_set_split_context(hub_addr, hub_port, 1, low_speed);
    }

    if (boot_kbd){
        // Boot protocol for fixed 8-byte reports.
        (void)usb_std_request(addr,
                              0x21,
                              USB_HID_REQ_SET_PROTOCOL,
                              0u,
                              iface,
                              0,
                              0);
    }

    // Request periodic reports while keys are held. A 4ms idle interval makes
    // the polling path tolerant of missed edge reports on simple split polling.
    (void)usb_std_request(addr,
                          0x21,
                          USB_HID_REQ_SET_IDLE,
                          (1u << 8),
                          iface,
                          0,
                          0);

    if (use_split){
        usb_clear_split_context();
    }

    g_kbd.present = 1;
    g_kbd.addr = addr;
    g_kbd.iface = iface;
    g_kbd.in_ep = in_ep;
    g_kbd.in_mps = (in_mps != 0u) ? in_mps : USB_HID_REPORT_LEN;
    g_kbd.hub_addr = hub_addr;
    g_kbd.hub_port = hub_port;
    g_kbd.use_split = use_split ? 1 : 0;
    g_kbd.low_speed = low_speed ? 1 : 0;
    g_kbd.boot_kbd = boot_kbd ? 1 : 0;
    g_kbd.in_toggle = 0;
    g_kbd.have_prev_report = 0;
    g_kbd.last_report_tick = system_ticks;
    g_kbd_active_until_tick = 0;
    for (unsigned int i = 0; i < USB_HID_REPORT_LEN; i++){
        g_kbd.prev_report[i] = 0;
    }
    g_kbd_next_poll_tick = 0;
    usb_hid_queue_reset();
    return 0;
}

static int usb_hid_poll_once(void){
    if (!g_kbd.present || g_kbd.addr == 0u){
        return 0;
    }

    unsigned int report_cap = g_kbd.in_mps;
    if (report_cap < USB_HID_REPORT_LEN){
        report_cap = USB_HID_REPORT_LEN;
    }
    if (report_cap > 16u){
        report_cap = 16u;
    }

    unsigned char report[16];
    for (unsigned int i = 0; i < report_cap; i++){
        report[i] = 0;
    }

    unsigned int actual = 0;
    g_root_info.hid_poll_count++;
    int rc = hc_transfer_interrupt_in(g_kbd.addr,
                                      g_kbd.in_ep,
                                      g_kbd.in_mps,
                                      report,
                                      report_cap,
                                      g_kbd.use_split,
                                      g_kbd.low_speed,
                                      g_kbd.hub_addr,
                                      g_kbd.hub_port,
                                      &actual);
    if (rc != 0){
        g_root_info.hid_error_count++;
        return -1;
    }
    g_root_info.hid_last_actual = actual;
    if (actual < 3u){
        g_root_info.hid_nodata_count++;
        if (g_kbd.have_prev_report && (long)(system_ticks - g_kbd.last_report_tick) > 30){
            for (unsigned int i = 0; i < USB_HID_REPORT_LEN; i++){
                g_kbd.prev_report[i] = 0;
            }
            g_kbd.have_prev_report = 0;
            g_kbd.last_report_tick = system_ticks;
            g_root_info.hid_stale_clear_count++;
        }
        return 0;
    }
    g_root_info.hid_report_count++;
    g_kbd_active_until_tick = system_ticks + USB_HID_ACTIVE_HOLD_MS;
    if (actual >= 9u && report[0] != 0u && report[1] == 0u){
        // Report-ID prefixed packet: decode the 8-byte boot layout after ID.
        usb_hid_process_report(&report[1]);
    } else{
        usb_hid_process_report(report);
    }
    return 0;
}

static void usb_parse_interrupt_in_endpoint_from_config(const unsigned char* cfg,
                                                       unsigned int len,
                                                       unsigned char* out_iface,
                                                       unsigned char* out_ep,
                                                       unsigned short* out_mps){
    unsigned char cur_iface = 0u;
    unsigned char ep = 0u;
    unsigned short mps = 0u;

    if (out_iface){ *out_iface = 0u; }
    if (out_ep){ *out_ep = 0u; }
    if (out_mps){ *out_mps = 0u; }
    if (!cfg || len < 9u){
        return;
    }

    unsigned int off = 0;
    while (off + 2u <= len){
        unsigned int desc_len = cfg[off];
        unsigned int desc_type = cfg[off + 1u];
        if (desc_len < 2u || off + desc_len > len){
            break;
        }

        if (desc_type == USB_DESC_TYPE_INTERFACE && desc_len >= 9u){
            cur_iface = cfg[off + 2u];
        } else if (desc_type == USB_DESC_TYPE_ENDPOINT && desc_len >= 7u){
            unsigned char ep_addr = cfg[off + 2u];
            unsigned char attrs = cfg[off + 3u];
            unsigned short this_mps = (unsigned short)(le16(&cfg[off + 4u]) & 0x7FFu);
            if ((attrs & 0x3u) == USB_ENDPOINT_XFER_INTERRUPT &&
                (ep_addr & 0x80u) &&
                this_mps >= 3u){
                ep = ep_addr;
                mps = this_mps;
                break;
            }
        }

        off += desc_len;
    }

    if (ep){
        if (out_iface){ *out_iface = cur_iface; }
        if (out_ep){ *out_ep = ep; }
        if (out_mps){ *out_mps = mps; }
    }
}

static void usb_parse_bulk_endpoints_from_config(const unsigned char* cfg,
                                                 unsigned int len,
                                                 unsigned char* out_in_ep,
                                                 unsigned short* out_in_mps,
                                                 unsigned char* out_out_ep,
                                                 unsigned short* out_out_mps){
    unsigned char in_ep = 0;
    unsigned char out_ep = 0;
    unsigned short in_mps = 0;
    unsigned short out_mps = 0;

    if (!cfg || len < 9){
        if (out_in_ep){ *out_in_ep = 0; }
        if (out_out_ep){ *out_out_ep = 0; }
        if (out_in_mps){ *out_in_mps = 0; }
        if (out_out_mps){ *out_out_mps = 0; }
        return;
    }

    unsigned int off = 0;
    while (off + 2 <= len){
        unsigned int desc_len = cfg[off];
        unsigned int desc_type = cfg[off + 1];
        if (desc_len < 2 || off + desc_len > len){
            break;
        }

        if (desc_type == USB_DESC_TYPE_ENDPOINT && desc_len >= 7){
            unsigned char ep_addr = cfg[off + 2];
            unsigned char attrs = cfg[off + 3];
            unsigned short mps = (unsigned short)(le16(&cfg[off + 4]) & 0x7FFu);
            if ((attrs & 0x3u) == USB_ENDPOINT_XFER_BULK && mps != 0){
                if ((ep_addr & 0x80u) && in_ep == 0){
                    in_ep = ep_addr;
                    in_mps = mps;
                } else if (!(ep_addr & 0x80u) && out_ep == 0){
                    out_ep = ep_addr;
                    out_mps = mps;
                }
            }
        }

        off += desc_len;
    }

    if (out_in_ep){ *out_in_ep = in_ep; }
    if (out_out_ep){ *out_out_ep = out_ep; }
    if (out_in_mps){ *out_in_mps = in_mps; }
    if (out_out_mps){ *out_out_mps = out_mps; }
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

    // Optional route for HID keyboard child behind the onboard hub.
    if (g_kbd.present &&
        g_kbd.use_split &&
        g_kbd.addr == dev_addr &&
        g_kbd.hub_addr != 0 &&
        g_kbd.hub_port != 0){
        *hub_addr = g_kbd.hub_addr;
        *hub_port = g_kbd.hub_port;
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

    if (g_kbd.present &&
        g_kbd.use_split &&
        g_kbd.low_speed &&
        g_kbd.addr == dev_addr){
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
    unsigned char cfg_value = 0;
    int cfg_set = 0;
    unsigned char bulk_in_ep = 0;
    unsigned char bulk_out_ep = 0;
    unsigned short bulk_in_mps = 0;
    unsigned short bulk_out_mps = 0;
    unsigned char hid_iface = 0;
    unsigned char hid_ep = 0;
    unsigned short hid_mps = 0;
    int hid_boot = 0;
    unsigned char intr_iface = 0;
    unsigned char intr_ep = 0;
    unsigned short intr_mps = 0;
    int hid_found = 0;
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

    int cfg_read = usb_get_config_descriptor(child_addr, cfg_desc, sizeof(cfg_desc), &cfg_total);
    if (cfg_read >= 6){
        cfg_value = cfg_desc[5];
        usb_parse_bulk_endpoints_from_config(cfg_desc,
                                             (unsigned int)cfg_read,
                                             &bulk_in_ep,
                                             &bulk_in_mps,
                                             &bulk_out_ep,
                                             &bulk_out_mps);
        usb_parse_interrupt_in_endpoint_from_config(cfg_desc,
                                                    (unsigned int)cfg_read,
                                                    &intr_iface,
                                                    &intr_ep,
                                                    &intr_mps);
        if (cfg_value != 0){
            if (usb_std_request(child_addr, 0x00, HUB_REQ_SET_CONFIGURATION, cfg_value, 0, 0, 0) == 0){
                cfg_set = 1;
            }
        }
        hid_found = (usb_parse_hid_keyboard_from_config(cfg_desc,
                                                        (unsigned int)cfg_read,
                                                        &hid_iface,
                                                        &hid_ep,
                                                        &hid_mps,
                                                        &hid_boot) == 0) ? 1 : 0;
        if (!hid_found && intr_ep && dev_desc[4] != 0x09u && !(bulk_in_ep && bulk_out_ep)){
            // Some low-cost keyboards report unusual interface metadata but still expose
            // a single interrupt-IN report endpoint. Avoid hubs and bulk NICs here.
            hid_found = 1;
            hid_iface = intr_iface;
            hid_ep = intr_ep;
            hid_mps = intr_mps;
            hid_boot = 0;
        }
        if (hid_found){
            g_hub_hid_candidates++;
            if (port < 32u){
                g_hub_hid_candidate_mask |= (1u << port);
            }
        }
    }

    if (port < USB_HOST_MAX_TRACKED_PORTS){
        g_port_addr[port] = child_addr;
        g_port_class[port] = dev_desc[4];
        g_port_config[port] = cfg_value;
        g_port_vid[port] = le16(&dev_desc[8]);
        g_port_pid[port] = le16(&dev_desc[10]);
        g_port_intr_in_ep[port] = intr_ep;
        g_port_intr_in_mps[port] = intr_mps;
    }

    // Preserve first child as the primary USB downstream function (used by NIC path).
    if (!g_root_info.child_present){
        g_root_info.child_present = 1;
        g_root_info.child_address = child_addr;
        g_root_info.child_class = dev_desc[4];
        g_root_info.child_vid = le16(&dev_desc[8]);
        g_root_info.child_pid = le16(&dev_desc[10]);
        g_root_info.child_config_value = cfg_value;
        g_root_info.child_configured = cfg_set ? 1 : 0;
        g_root_info.child_bulk_in_ep = bulk_in_ep;
        g_root_info.child_bulk_out_ep = bulk_out_ep;
        g_root_info.child_bulk_in_mps = bulk_in_mps;
        g_root_info.child_bulk_out_mps = bulk_out_mps;
        g_child_use_split = child_use_split;
        g_child_low_speed = child_is_low_speed;
        if (child_use_split){
            g_root_info.child_hub_address = hub_addr;
            g_root_info.child_hub_port = (unsigned char)port;
        } else{
            g_root_info.child_hub_address = 0;
            g_root_info.child_hub_port = 0;
        }
        if (cfg_set){
            usb_reset_bulk_toggles();
        }
    }

    if (hid_found && !g_kbd.present){
        g_root_info.child_hid_kbd_address = child_addr;
        g_root_info.child_hid_kbd_ep = hid_ep;
        g_root_info.child_hid_kbd_iface = hid_iface;
        g_root_info.child_hid_kbd_mps = hid_mps;
        g_root_info.child_hid_kbd_present = 0;
        if (usb_hid_keyboard_configure(child_addr,
                                       hid_iface,
                                       hid_ep,
                                       hid_mps,
                                       hid_boot,
                                       child_use_split,
                                       child_is_low_speed,
                                       hub_addr,
                                       (unsigned char)port) == 0){
            g_root_info.child_hid_kbd_present = 1;
            uart_puts("USB: HID keyboard configured addr=");
            uart_puthex(child_addr);
            uart_puts(" iface=");
            uart_puthex(hid_iface);
            uart_puts(" boot=");
            uart_puthex((unsigned int)(hid_boot ? 1u : 0u));
            uart_puts("\n");
        } else{
            uart_puts("USB: HID keyboard configure failed addr=");
            uart_puthex(child_addr);
            uart_puts(" iface=");
            uart_puthex(hid_iface);
            uart_puts("\n");
        }
    }

    uart_puts("USB: hub child addr=");
    uart_puthex(child_addr);
    uart_puts(" vid=");
    uart_puthex(le16(&dev_desc[8]));
    uart_puts(" pid=");
    uart_puthex(le16(&dev_desc[10]));
    uart_puts(" class=");
    uart_puthex(dev_desc[4]);
    uart_puts(" cfg=");
    uart_puthex(cfg_value);
    uart_puts(cfg_set ? " (set)\n" : " (not set)\n");
    if (bulk_in_ep || bulk_out_ep){
        uart_puts("USB: child bulk in=");
        uart_puthex(bulk_in_ep);
        uart_puts(" mps=");
        uart_puthex(bulk_in_mps);
        uart_puts(" out=");
        uart_puthex(bulk_out_ep);
        uart_puts(" mps=");
        uart_puthex(bulk_out_mps);
        uart_puts("\n");
    }
    usb_clear_split_context();
    return 0;
}

static void spin_delay(unsigned int n){
    while (n--){
        asm volatile("nop");
    }
}

static void usb_wait_microframes(unsigned int count){
    unsigned int last = HFNUM & 0xFFFFu;
    unsigned int guard = 200000u + (count * 400000u);
    while (count && guard--){
        unsigned int cur = HFNUM & 0xFFFFu;
        if (cur != last){
            last = cur;
            count--;
        }
    }
    if (count){
        spin_delay(50000u * count);
    }
}

static unsigned int usb_microframe(void){
    return HFNUM & 0x7u;
}

static void usb_wait_for_microframe(unsigned int target){
    target &= 0x7u;
    unsigned int guard = 1200000u;
    while ((usb_microframe() != target) && guard--){
        asm volatile("nop");
    }
}

static unsigned int usb_next_data_pid(unsigned int pid){
    return (pid == HCTSIZ_PID_DATA1) ? HCTSIZ_PID_DATA0 : HCTSIZ_PID_DATA1;
}

static int usb_valid_ep0_mps(unsigned int mps){
    return mps == 8u || mps == 16u || mps == 32u || mps == 64u;
}

static unsigned long usb_cache_line_size(void){
    unsigned long ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4UL << ((ctr >> 16) & 0xFUL);
}

static void usb_dcache_clean_invalidate_range(unsigned long start, unsigned long size){
    if (size == 0){
        return;
    }
    unsigned long line = usb_cache_line_size();
    unsigned long addr = start & ~(line - 1);
    unsigned long end = (start + size + line - 1) & ~(line - 1);
    for (; addr < end; addr += line){
        asm volatile("dc civac, %0" : : "r"(addr) : "memory");
    }
    asm volatile("dsb ish");
}

static void usb_dcache_invalidate_range(unsigned long start, unsigned long size){
    if (size == 0){
        return;
    }
    unsigned long line = usb_cache_line_size();
    unsigned long addr = start & ~(line - 1);
    unsigned long end = (start + size + line - 1) & ~(line - 1);
    for (; addr < end; addr += line){
        asm volatile("dc ivac, %0" : : "r"(addr) : "memory");
    }
    asm volatile("dsb ish");
}

static unsigned int usb_bus_address(const void* p){
    unsigned long addr = (unsigned long)p;
    return (unsigned int)((addr & ~0xC0000000UL) | GPU_UNCACHED_BASE);
}

static void usb_copy_to_dma(const unsigned char* src, unsigned int len){
    for (unsigned int i = 0; i < len; i++){
        g_usb_dma_buffer[i] = src ? src[i] : 0;
    }
}

static void usb_copy_from_dma(unsigned char* dst, unsigned int len){
    if (!dst){
        return;
    }
    for (unsigned int i = 0; i < len; i++){
        dst[i] = g_usb_dma_buffer[i];
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

    // Known-good DWC2 halt flow used by U-Boot/coreboot style drivers:
    // request CHDIS while keeping CHENA asserted so the core generates CHHLTD.
    HCCHAR(ch) = hcchar | HCCHAR_CHDIS | HCCHAR_CHENA;
    if (hc_wait_idle(ch, 500000) == 0){
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = 0;
        HCSPLT(ch) = 0;
        return 0;
    }

    // One recovery flush/retry path for cores that occasionally wedge CH0.
    usb_flush_host_fifos();
    hcchar = HCCHAR(ch);
    HCCHAR(ch) = hcchar | HCCHAR_CHDIS | HCCHAR_CHENA;
    if (hc_wait_idle(ch, 500000) == 0){
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = 0;
        HCSPLT(ch) = 0;
        return 0;
    }

    HCINT(ch) = 0xFFFFFFFFu;
    HCINTMSK(ch) = 0;
    return -1;
}

static void usb_halt_all_channels(void){
    for (unsigned int ch = 0; ch < USB_DWC2_CHANNELS; ch++){
        HCINTMSK(ch) = 0;
        HCINT(ch) = 0xFFFFFFFFu;
        HCSPLT(ch) = 0;
        HCCHAR(ch) = (HCCHAR(ch) & ~HCCHAR_EPDIR) | HCCHAR_CHDIS;
    }
    for (unsigned int ch = 0; ch < USB_DWC2_CHANNELS; ch++){
        unsigned int hcchar = (HCCHAR(ch) & ~HCCHAR_EPDIR) | HCCHAR_CHENA | HCCHAR_CHDIS;
        HCCHAR(ch) = hcchar;
        (void)hc_wait_idle(ch, 500000);
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = 0;
        HCSPLT(ch) = 0;
    }
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
static int hc_wait_for_done(unsigned int ch,
                            int is_in,
                            unsigned char* in_buf,
                            unsigned int in_len,
                            unsigned int* out_hcint){
    unsigned int copied = 0;
    unsigned int loops = 10000000;
    if (out_hcint){
        *out_hcint = 0;
    }

    while (loops--){
        if (!g_usb_dma_mode && is_in && (GINTSTS & GINTSTS_RXFLVL)){
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
        if (hcint & HCINT_CHHLTD){
            unsigned int snap = HCINT(ch);
            HCINT(ch) = snap;
            if (out_hcint){
                *out_hcint = snap;
            }
            if (snap & (HCINT_XFERCOMPL | HCINT_ACK)){
                return 0;
            }
            if (snap & (HCINT_NAK | HCINT_NYET | HCINT_FRMOVRUN | HCINT_XACTERR)){
                return 1;
            }
            return -1;
        }

        // On the BCM DWC2 core we sometimes see XFERCOMPL|ACK with CHENA
        // already cleared but without a CHHLTD bit. Treat that as a completed
        // stage; otherwise successful SETUP packets look like hard failures.
        if ((hcint & (HCINT_XFERCOMPL | HCINT_ACK)) &&
            ((HCCHAR(ch) & HCCHAR_CHENA) == 0)){
            HCINT(ch) = hcint;
            if (out_hcint){
                *out_hcint = hcint;
            }
            return 0;
        }

        // Some DWC2 variants expose retryable NAK/FRMOVRUN without CHHLTD.
        // Abort and return retry so caller can resubmit transaction cleanly.
        if (hcint & (HCINT_NAK | HCINT_FRMOVRUN)){
            if (out_hcint){
                *out_hcint = hcint;
            }
            (void)hc_force_halt(ch);
            return 1;
        }
    }

    if (out_hcint){
        *out_hcint = HCINT(ch);
    }
    (void)hc_force_halt(ch);
    return -1;
}

static int hc_transfer_reg(unsigned int ch,
                           unsigned char dev_addr,
                           unsigned int ep_num,
                           unsigned int ep_type,
                           int ep_in,
                           unsigned int ep_mps,
                           unsigned int pid,
                           const unsigned char* out_data,
                           unsigned int out_len,
                           unsigned char* in_data,
                           unsigned int in_len,
                           unsigned int hcsplt_reg,
                           int quiet,
                           unsigned int* actual_out){
    if (ep_mps == 0){
        ep_mps = USB_CTRL_EP_MPS_DEFAULT;
    }
    if (actual_out){
        *actual_out = 0;
    }

    unsigned int xfer_len = ep_in ? in_len : out_len;
    unsigned int pktcnt = (xfer_len == 0) ? 1u : div_round_up(xfer_len, ep_mps ? ep_mps : 1u);
    if (pktcnt == 0){
        pktcnt = 1;
    }
    unsigned int programmed_len = xfer_len;
    if (ep_in && xfer_len > 0){
        programmed_len = pktcnt * ep_mps;
    }
    if (programmed_len > USB_DMA_BUFFER_SIZE){
        uart_puts("USB: DMA buffer too small\n");
        return -1;
    }

    // Ensure channel is idle before programming a new transfer.
    if (hc_wait_idle(ch, 500000) != 0){
        if (hc_force_halt(ch) != 0){
            if (!quiet && (g_preidle_fail_logs < 4 || (g_preidle_fail_logs & 0x3Fu) == 0u)){
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

    unsigned int retry_hcint = 0;
    unsigned int max_attempts = quiet ? 2u : 64u;
    for (unsigned int attempt = 0; attempt < max_attempts; attempt++){
        if (attempt > 0){
            if (hc_force_halt(ch) != 0){
                return -1;
            }
        }
        HCINT(ch) = 0xFFFFFFFFu;
        HAINTMSK |= (1u << ch);
        GINTMSK |= GINTSTS_HCHINT;
        HCINTMSK(ch) = HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_ERROR_MASK |
            HCINT_NAK | HCINT_ACK | HCINT_NYET | HCINT_XACTERR | HCINT_FRMOVRUN;
        HCSPLT(ch) = hcsplt_reg;

        unsigned int hctsiz = (programmed_len & HCTSIZ_XFERSIZE_MASK)
            | (pktcnt << HCTSIZ_PKTCNT_SHIFT)
            | ((pid & 0x3u) << HCTSIZ_PID_SHIFT);
        HCTSIZ(ch) = hctsiz;

        if (g_usb_dma_mode){
            if (ep_in){
                usb_copy_to_dma(0, programmed_len);
            } else if (out_len > 0){
                usb_copy_to_dma(out_data, out_len);
            } else{
                usb_copy_to_dma(0, 4);
            }
            usb_dcache_clean_invalidate_range((unsigned long)g_usb_dma_buffer,
                                              programmed_len ? programmed_len : 4);
            HCDMA(ch) = usb_bus_address(g_usb_dma_buffer);
        }

        unsigned int hcchar = (ep_mps & HCCHAR_MPS_MASK)
            | ((ep_num & 0xFu) << HCCHAR_EPNUM_SHIFT)
            | ((dev_addr & 0x7Fu) << HCCHAR_DEVADDR_SHIFT)
            | ((ep_type & 0x3u) << HCCHAR_EPTYPE_SHIFT)
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

        if (!g_usb_dma_mode && !ep_in && out_len > 0 && out_data){
            fifo_write_bytes(out_data, out_len);
        }

        hcchar |= HCCHAR_CHENA;
        hcchar &= ~HCCHAR_CHDIS;
        HCCHAR(ch) = hcchar;

        unsigned int done_hcint = 0;
        int rc = hc_wait_for_done(ch, ep_in, in_data, in_len, &done_hcint);
        if (rc == 0){
            unsigned int actual = xfer_len;
            if (g_usb_dma_mode && ep_in && in_data && in_len > 0){
                usb_dcache_invalidate_range((unsigned long)g_usb_dma_buffer, programmed_len);
                unsigned int remaining = HCTSIZ(ch) & HCTSIZ_XFERSIZE_MASK;
                actual = (programmed_len >= remaining) ? (programmed_len - remaining) : in_len;
                if (actual > in_len){
                    actual = in_len;
                }
                usb_copy_from_dma(in_data, actual);
            }
            if (ep_in && in_data && in_len > 0 && actual == 0){
                retry_hcint = done_hcint;
                if (hc_force_halt(ch) != 0){
                    return -1;
                }
                spin_delay(50000);
                continue;
            }
            if (actual_out){
                *actual_out = actual;
            }
            g_preidle_fail_logs = 0;
            return 0;
        }
        if (rc < 0){
            if (!quiet){
                uart_puts("USB: hc xfer hard fail hcint=");
                uart_puthex(done_hcint);
                uart_puts(" hctsiz=");
                uart_puthex(HCTSIZ(ch));
                uart_puts(" hcchar=");
                uart_puthex(HCCHAR(ch));
                uart_puts(" hcsplt=");
                uart_puthex(HCSPLT(ch));
                uart_puts("\n");
            }
            (void)hc_force_halt(ch);
            return -1;
        }
        // Retryable NAK/NYET/XACTERR path.
        retry_hcint = done_hcint;
        if (hc_force_halt(ch) != 0){
            return -1;
        }
        spin_delay(50000);
    }

    if (!quiet){
        uart_puts("USB: hc xfer retry exhausted\n");
    }
    return (retry_hcint & (HCINT_NAK | HCINT_NYET)) ? -2 : -1;
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
    return hc_transfer_reg(ch, dev_addr, 0, HC_EPTYPE_CONTROL, ep_in, ep_mps, pid,
                           out_data, out_len, in_data, in_len, 0u, 0, 0);
}

static int hc_transfer_bulk(unsigned char dev_addr,
                            unsigned char ep_addr,
                            unsigned int ep_mps,
                            unsigned char* data,
                            unsigned int len,
                            int in_transfer,
                            unsigned int* actual_out){
    unsigned int ep_num = ep_addr & 0x0Fu;
    if (ep_num == 0 || !data || len == 0){
        return -1;
    }
    if (((ep_addr & 0x80u) ? 1 : 0) != (in_transfer ? 1 : 0)){
        return -1;
    }

    unsigned char* toggle = in_transfer ? &g_bulk_in_toggle[ep_num] : &g_bulk_out_toggle[ep_num];
    unsigned int pid = *toggle ? HCTSIZ_PID_DATA1 : HCTSIZ_PID_DATA0;
    unsigned int actual = 0;
    int rc = hc_transfer_reg(0, dev_addr, ep_num, HC_EPTYPE_BULK, in_transfer ? 1 : 0,
                             ep_mps, pid,
                             in_transfer ? 0 : data,
                             in_transfer ? 0 : len,
                             in_transfer ? data : 0,
                             in_transfer ? len : 0,
                             0u,
                             in_transfer ? 1 : 0,
                             &actual);
    if (rc == -2 && in_transfer){
        if (actual_out){
            *actual_out = 0;
        }
        return 0;
    }
    if (rc != 0){
        return -1;
    }

    unsigned int bytes = in_transfer ? actual : len;
    unsigned int packets = (bytes == 0) ? 1u : div_round_up(bytes, ep_mps ? ep_mps : 1u);
    if (packets & 1u){
        *toggle ^= 1u;
    }
    if (actual_out){
        *actual_out = bytes;
    }
    return 0;
}

static int hc_transfer_split_in_packet(unsigned int ch,
                                       unsigned char dev_addr,
                                       unsigned int ep_num,
                                       unsigned int ep_type,
                                       unsigned int ep_mps,
                                       unsigned int pid,
                                       unsigned char* in_data,
                                       unsigned int in_len,
                                       unsigned char hub_addr,
                                       unsigned char hub_port,
                                       unsigned int* actual_out){
    if (actual_out){
        *actual_out = 0;
    }
    if (!in_data || in_len == 0u){
        return -1;
    }
    if (ep_mps == 0u){
        ep_mps = USB_CTRL_EP_MPS_DEFAULT;
    }

    unsigned int want = in_len;
    if (want > ep_mps){
        want = ep_mps;
    }

    unsigned int split_reg = HCSPLT_SPLTENA
        | ((HCSPLT_XACTPOS_ALL & 0x3u) << HCSPLT_XACTPOS_SHIFT)
        | (((unsigned int)hub_addr & 0x7Fu) << HCSPLT_HUBADDR_SHIFT)
        | (((unsigned int)hub_port & 0x7Fu) << HCSPLT_PRTADDR_SHIFT);

    // Periodic split schedule, following Circle's DWC2 pattern:
    // SSPLIT in the next microframe (skip 6), CSPLIT two microframes later,
    // then advance one microframe for each retry.
    unsigned int start_mf = (usb_microframe() + 1u) & 0x7u;
    if (start_mf == 6u){
        start_mf = 7u;
    }
    usb_wait_for_microframe(start_mf);
    if (hc_transfer_reg(ch, dev_addr, ep_num, ep_type, 1, ep_mps, pid,
                        0, 0, 0, want, split_reg, 1, 0) != 0){
        HCSPLT(ch) = 0;
        return -1;
    }

    unsigned int complete_mf = (start_mf + 2u) & 0x7u;
    usb_wait_for_microframe(complete_mf);
    for (unsigned int tries = 0; tries < 8; tries++){
        unsigned int actual = 0;
        int rc = hc_transfer_reg(ch, dev_addr, ep_num, ep_type, 1, ep_mps, pid,
                                 0, 0, in_data, want,
                                 split_reg | HCSPLT_COMPSPLT,
                                 1,
                                 &actual);
        if (rc == 0 && actual > 0u){
            HCSPLT(ch) = 0;
            if (actual_out){
                *actual_out = actual;
            }
            return 0;
        }
        if (rc == -1){
            HCSPLT(ch) = 0;
            return -1;
        }
        complete_mf = (complete_mf + 1u) & 0x7u;
        usb_wait_for_microframe(complete_mf);
    }

    // Interrupt IN endpoints normally NAK when no key state changed.
    HCSPLT(ch) = 0;
    return 0;
}

static int hc_transfer_interrupt_in(unsigned char dev_addr,
                                    unsigned char ep_addr,
                                    unsigned int ep_mps,
                                    unsigned char* data,
                                    unsigned int len,
                                    int use_split,
                                    int low_speed,
                                    unsigned char hub_addr,
                                    unsigned char hub_port,
                                    unsigned int* actual_out){
    if (actual_out){
        *actual_out = 0;
    }
    if (!data || len == 0u || !(ep_addr & 0x80u)){
        return -1;
    }

    unsigned int ep_num = ep_addr & 0x0Fu;
    if (ep_num == 0u){
        return -1;
    }
    if (ep_mps == 0u){
        ep_mps = USB_HID_REPORT_LEN;
    }

    unsigned int pid = g_kbd.in_toggle ? HCTSIZ_PID_DATA1 : HCTSIZ_PID_DATA0;
    unsigned int actual = 0;
    int rc;

    if (use_split){
        usb_set_split_context(hub_addr, hub_port, 1, low_speed);
        rc = hc_transfer_split_in_packet(0,
                                         dev_addr,
                                         ep_num,
                                         HC_EPTYPE_INTERRUPT,
                                         ep_mps,
                                         pid,
                                         data,
                                         len,
                                         hub_addr,
                                         hub_port,
                                         &actual);
        usb_clear_split_context();
    } else{
        rc = hc_transfer_reg(0,
                             dev_addr,
                             ep_num,
                             HC_EPTYPE_INTERRUPT,
                             1,
                             ep_mps,
                             pid,
                             0, 0,
                             data, len,
                             0u,
                             1,
                             &actual);
        if (rc == -2){
            actual = 0;
            rc = 0;
        }
    }

    if (rc != 0){
        return -1;
    }

    if (actual > len){
        actual = len;
    }
    if (actual > 0u){
        unsigned int packets = div_round_up(actual, ep_mps ? ep_mps : 1u);
        if (packets & 1u){
            g_kbd.in_toggle ^= 1u;
        }
    }
    if (actual_out){
        *actual_out = actual;
    }
    return 0;
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
    if (ep_mps == 0){
        ep_mps = USB_CTRL_EP_MPS_DEFAULT;
    }

    unsigned int split_reg = HCSPLT_SPLTENA
        | ((HCSPLT_XACTPOS_ALL & 0x3u) << HCSPLT_XACTPOS_SHIFT)
        | (((unsigned int)hub_addr & 0x7Fu) << HCSPLT_HUBADDR_SHIFT)
        | (((unsigned int)hub_port & 0x7Fu) << HCSPLT_PRTADDR_SHIFT);

    if (ep_in){
        unsigned int done = 0;
        unsigned int cur_pid = pid;

        do {
            unsigned int want = in_len - done;
            if (want > ep_mps){
                want = ep_mps;
            }

            // Circle schedules split transactions as SSPLIT, delayed CSPLIT,
            // and repeats SSPLIT again if the transfer stage is not complete.
            usb_wait_microframes(1);
            if (hc_transfer_reg(ch, dev_addr, 0, HC_EPTYPE_CONTROL, 1, ep_mps, cur_pid,
                                0, 0, 0, want, split_reg, 1, 0) != 0){
                HCSPLT(ch) = 0;
                return -1;
            }

            usb_wait_microframes(2);
            unsigned int actual = 0;
            int packet_ok = 0;
            for (unsigned int tries = 0; tries < 8; tries++){
                actual = 0;
                int rc = hc_transfer_reg(ch, dev_addr, 0, HC_EPTYPE_CONTROL, 1, ep_mps, cur_pid,
                                         0, 0,
                                         in_data ? &in_data[done] : 0,
                                         want,
                                         split_reg | HCSPLT_COMPSPLT,
                                         1,
                                         &actual);
                if (rc == 0 && (want == 0 || actual > 0)){
                    packet_ok = 1;
                    break;
                }
                usb_wait_microframes(5);
            }
            if (!packet_ok){
                HCSPLT(ch) = 0;
                return -1;
            }
            if (want == 0){
                HCSPLT(ch) = 0;
                return 0;
            }

            done += actual;
            if (actual < want){
                HCSPLT(ch) = 0;
                return 0;
            }
            cur_pid = usb_next_data_pid(cur_pid);
        } while (done < in_len);

        HCSPLT(ch) = 0;
        return 0;
    } else{
        unsigned int done = 0;
        unsigned int cur_pid = pid;

        do {
            unsigned int want = out_len - done;
            if (want > ep_mps){
                want = ep_mps;
            }

            usb_wait_microframes(1);
            if (hc_transfer_reg(ch, dev_addr, 0, HC_EPTYPE_CONTROL, 0, ep_mps, cur_pid,
                                out_data ? &out_data[done] : 0,
                                want,
                                0, 0,
                                split_reg,
                                1,
                                0) != 0){
                HCSPLT(ch) = 0;
                return -1;
            }

            usb_wait_microframes(2);
            int packet_ok = 0;
            for (unsigned int tries = 0; tries < 8; tries++){
                int rc = hc_transfer_reg(ch, dev_addr, 0, HC_EPTYPE_CONTROL, 0, ep_mps, cur_pid,
                                         g_status_dummy, 0, 0, 0,
                                         split_reg | HCSPLT_COMPSPLT,
                                         1,
                                         0);
                if (rc == 0){
                    packet_ok = 1;
                    break;
                }
                usb_wait_microframes(5);
            }
            if (!packet_ok){
                HCSPLT(ch) = 0;
                return -1;
            }
            if (want == 0){
                HCSPLT(ch) = 0;
                return 0;
            }

            done += want;
            cur_pid = usb_next_data_pid(cur_pid);
        } while (done < out_len);

        HCSPLT(ch) = 0;
        return 0;
    }

    HCSPLT(ch) = 0;
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
    for (unsigned int i = 0; i < sizeof(g_root_info); i++){
        ((unsigned char*)&g_root_info)[i] = 0;
    }
    for (unsigned int i = 0; i < sizeof(g_kbd); i++){
        ((unsigned char*)&g_kbd)[i] = 0;
    }
    g_kbd_next_poll_tick = 0;
    usb_hid_queue_reset();
    g_child_use_split = 0;
    g_child_low_speed = 0;
    usb_reset_hub_diag();
    usb_reset_bulk_toggles();

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
    HPTXFSIZ = (256u << 16) | 768u;  // periodic depth | start addr

    // Clear and mask interrupts for phase 1 polling path.
    GINTSTS = 0xFFFFFFFFu;
    GINTMSK = GINTSTS_HCHINT;
    HAINTMSK = 0xFFFFFFFFu;
    // Circle/U-Boot style path: use DWC2 internal DMA with a small EP0 bounce buffer.
    GAHBCFG |= GAHBCFG_GLBL_INTR_EN | GAHBCFG_DMA_EN;

    // Match common DWC2 HS PHY host setup used by Linux/U-Boot on BCM SoCs.
    HCFG = (HCFG & ~HCFG_FSLSPCLKSEL_MASK) | HCFG_FSLSPCLKSEL_30_60_MHZ;
    (void)HFIR;
    usb_halt_all_channels();

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

int usb_host_bulk_transfer(unsigned char dev_addr,
                           unsigned char ep_addr,
                           unsigned int ep_mps,
                           unsigned char* data,
                           unsigned int len,
                           int in_transfer){
    if (!g_usb_ready || !data || len == 0){
        return -1;
    }
    if (!(HPRT0 & HPRT0_ENA)){
        return -1;
    }

    unsigned int actual = 0;
    if (hc_transfer_bulk(dev_addr, ep_addr, ep_mps, data, len, in_transfer, &actual) != 0){
        return -1;
    }
    return (int)actual;
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
    if (out18[0] < 8u || out18[1] != 0x01u || !usb_valid_ep0_mps(out18[7])){
        return -1;
    }
    g_ep0_mps = out18[7];

    // Full descriptor with established EP0 MPS.
    if (usb_std_request(dev_addr, 0x80, 0x06, 0x0100, 0x0000, out18, 18) != 0){
        return -1;
    }
    if (out18[0] != 18u || out18[1] != 0x01u || !usb_valid_ep0_mps(out18[7])){
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
    if (buf[0] != 9u || buf[1] != 0x02u){
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
    if (buf[0] != 9u || buf[1] != 0x02u || le16(&buf[2]) != total){
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
    for (unsigned int i = 0; i < sizeof(g_kbd); i++){
        ((unsigned char*)&g_kbd)[i] = 0;
    }
    g_kbd_next_poll_tick = 0;
    usb_hid_queue_reset();
    g_child_use_split = 0;
    g_child_low_speed = 0;
    usb_reset_hub_diag();

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
            usb_reset_bulk_toggles();
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
            g_hub_ports = ports;
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
                        if (st & HUB_PORT_STAT_CONNECTION){
                            if (port < 32u){
                                g_hub_connected_mask |= (1u << port);
                            }
                        }
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
                    if (port >= 32u || !(g_hub_connected_mask & (1u << port))){
                        continue;
                    }
                    g_hub_enum_attempts++;
                    if (usb_enumerate_hub_downstream_child(g_root_info.address, (unsigned short)port, next_addr) == 0){
                        child_found = 1;
                        g_hub_enum_success++;
                        if (port < 32u){
                            g_hub_enum_success_mask |= (1u << port);
                        }
                        next_addr++;
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
    usb_snapshot_hub_diag_to_root_info();
    return 0;
}

int usb_host_get_root_device_info(usb_root_device_info_t* out_info){
    if (!out_info || !g_root_info.present){
        return -1;
    }
    *out_info = g_root_info;
    return 0;
}

void usb_host_poll(void){
    if (!g_usb_ready || !g_kbd.present){
        return;
    }
    unsigned long now = system_ticks;
    if ((long)(now - g_kbd_next_poll_tick) < 0){
        return;
    }
    (void)usb_hid_poll_once();
    now = system_ticks;
    // Polling a HID keyboard behind the Pi 3 LAN9514 hub requires split
    // transactions. Poll slowly while idle, then briefly speed up after any
    // report so normal typing stays responsive without taxing graphics loops.
    unsigned int interval = ((long)(now - g_kbd_active_until_tick) < 0) ?
                            USB_HID_ACTIVE_POLL_MS :
                            USB_HID_IDLE_POLL_MS;
    g_kbd_next_poll_tick = now + interval;
}

int usb_host_try_getc(char* out){
    return usb_hid_queue_pop(out);
}

static void usb_snapshot_hub_diag_to_root_info(void){
    g_root_info.hub_ports = (unsigned char)(g_hub_ports & 0xFFu);
    g_root_info.hub_enum_attempts = (unsigned char)(g_hub_enum_attempts & 0xFFu);
    g_root_info.hub_enum_success = (unsigned char)(g_hub_enum_success & 0xFFu);
    g_root_info.hub_hid_candidates = (unsigned char)(g_hub_hid_candidates & 0xFFu);
    g_root_info.hub_connected_mask = g_hub_connected_mask;
    g_root_info.hub_enum_success_mask = g_hub_enum_success_mask;
    g_root_info.hub_hid_candidate_mask = g_hub_hid_candidate_mask;
    for (unsigned int i = 0; i < USB_HOST_MAX_TRACKED_PORTS; i++){
        g_root_info.port_addr[i] = g_port_addr[i];
        g_root_info.port_class[i] = g_port_class[i];
        g_root_info.port_config[i] = g_port_config[i];
        g_root_info.port_intr_in_ep[i] = g_port_intr_in_ep[i];
        g_root_info.port_vid[i] = g_port_vid[i];
        g_root_info.port_pid[i] = g_port_pid[i];
        g_root_info.port_intr_in_mps[i] = g_port_intr_in_mps[i];
    }
}
