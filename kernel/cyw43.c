#include "cyw43.h"
#include "sdio_bus.h"
#include "fat32.h"
#include "blockdev.h"
#include "memory.h"
#include "uart.h"
#include "net.h"
#include "net_proto.h"
#include "arp.h"
#include "spinlock.h"

extern volatile unsigned long system_ticks;

#define CYW43_FW_MAX_BYTES      (768u * 1024u)
#define CYW43_NVRAM_MAX_BYTES   (16u * 1024u)
#define CYW43_NVRAM_PACKED_MAX  (20u * 1024u)
#define CYW43_CLM_MAX_BYTES     (96u * 1024u)
#define CYW43_SDIO_XFER_CHUNK   64u

// Circle/ether4330 backplane access constants.
#define CYW43_SB_WINDOW_SIZE    0x8000u
#define CYW43_SB_32BIT_ADDR     0x8000u
#define CYW43_SB_ADDR_REG       0x1000Au
#define CYW43_CLKCSR_REG        0x1000Eu
#define CYW43_CLK_REQ_ALP       0x08u
#define CYW43_CLK_FORCE_ALP     0x01u
#define CYW43_CLK_REQ_HT        0x10u
#define CYW43_CLK_FORCE_HT      0x02u
#define CYW43_CLK_ALP_AVAIL     0x40u
#define CYW43_CLK_HT_AVAIL      0x80u
#define CYW43_CLK_NO_HW_REQ     0x20u
#define CYW43_ENUM_BASE         0x18000000u
#define CYW43_WATERMARK_REG     0x10008u
#define CYW43_FRAMECTL_REG      0x1000Du
#define CYW43_RFRAME_COUNT_REG  0x1001Bu
#define CYW43_SLEEP_CSR_REG     0x1001Fu
#define CYW43_SLEEP_CSR_KSO     0x01u
#define CYW43_SLEEP_CSR_DEVON   0x02u
#define CYW43_FRAMECTL_RFHALT   0x01u
#define CYW43_DEFAULT_F2_WM     0x08u

#define CYW43_CORE_ARM_CM3      0x82Au
#define CYW43_CORE_ARM_CR4      0x83Eu
#define CYW43_CORE_D11          0x812u
#define CYW43_CORE_SOCSRAM      0x80Eu
#define CYW43_CORE_SDIO         0x829u

#define CYW43_CORE_IOCTRL       0x408u
#define CYW43_CORE_RESETCTRL    0x800u
#define CYW43_SOCRAM_COREINFO   0x00u
#define CYW43_SOCRAM_BANKIDX    0x10u
#define CYW43_SOCRAM_BANKINFO   0x40u
#define CYW43_SOCRAM_BANKPDA    0x44u
#define CYW43_SD_INT_STATUS     0x20u
#define CYW43_SD_INT_MASK       0x24u
#define CYW43_SD_SBMBOX         0x40u
#define CYW43_SD_SBMBOX_DATA    0x48u
#define CYW43_SD_HOSTMBOX_DATA  0x4Cu
#define CYW43_SD_INT_FC_STATE   (1u << 4)
#define CYW43_SD_INT_FC_CHANGE  (1u << 5)
#define CYW43_SD_INT_FRAME      (1u << 6)
#define CYW43_SD_INT_MAILBOX    (1u << 7)
#define CYW43_SD_INT_HOST_MASK  0x000000F0u
#define CYW43_SD_INT_XMTDATA_AVAIL (1u << 23)
#define CYW43_SD_INT_CHIPACTIVE (1u << 29)
#define CYW43_SD_HOST_INT_MASK  (CYW43_SD_INT_HOST_MASK | CYW43_SD_INT_CHIPACTIVE)
#define CYW43_HMB_DATA_NAKHANDLED 0x0001u
#define CYW43_HMB_DATA_DEVREADY   0x0002u
#define CYW43_HMB_DATA_FC         0x0004u
#define CYW43_HMB_DATA_FWREADY    0x0008u
#define CYW43_HMB_DATA_FWHALT     0x0010u
#define CYW43_HMB_DATA_VERSION_MASK 0x00FF0000u
#define CYW43_HMB_DATA_VERSION_SHIFT 16u
#define CYW43_HMB_DATA_READY_MASK (CYW43_HMB_DATA_DEVREADY | CYW43_HMB_DATA_FWREADY)
#define CYW43_SD_FW_READY         0x80u
#define CYW43_SD_FW_READY_ALT     0x08u // Circle's intwait path treats this as firmware-ready.

#define CYW43_CRESCAN_SIZE      512u
#define CYW43_SDPCM_HDR_LEN     12u
#define CYW43_SDPCM_FIRSTREAD   64u
#define CYW43_CDC_HDR_LEN       16u
#define CYW43_PACKET_MAX_BYTES  4096u
#define CYW43_RAW_CAPTURE_DEPTH 64u
#define CYW43_RAW_CAPTURE_MAX_BYTES 2304u
#define CYW43_PACKET_ADDR       0u
#define CYW43_DATA_PAD_LEN      2u
#define CYW43_BDC_HEADER_LEN    4u
#define CYW43_BDC_PROTO_VER     2u
#define CYW43_BDC_VER_SHIFT     4u
#define CYW43_WLC_UP            2u
#define CYW43_WLC_DOWN          3u
#define CYW43_WLC_SET_INFRA     20u
#define CYW43_WLC_SET_AUTH      22u
#define CYW43_WLC_SET_SSID      26u
#define CYW43_WLC_SET_CHANNEL   30u
#define CYW43_WLC_SET_PASSIVE_SCAN 49u
#define CYW43_WLC_SCAN          50u
#define CYW43_WLC_SCAN_RESULTS  51u
#define CYW43_WLC_GET_RADIO     37u
#define CYW43_WLC_SET_RADIO     38u
#define CYW43_WLC_SET_ANTDIV    64u
#define CYW43_WLC_SET_COUNTRY   84u
#define CYW43_WLC_GET_PROMISC   9u
#define CYW43_WLC_SET_PROMISC   10u
#define CYW43_WLC_GET_MONITOR   107u
#define CYW43_WLC_SET_MONITOR   108u
#define CYW43_WLC_GET_SCANSUPPRESS 115u
#define CYW43_WLC_SET_SCANSUPPRESS 116u
#define CYW43_WLC_SET_WSEC      134u
#define CYW43_WLC_SET_WPA_AUTH  165u
#define CYW43_WLC_GET_UP        162u
#define CYW43_WLC_SET_SCAN_CHANNEL_TIME 185u
#define CYW43_WLC_SET_SCAN_UNASSOC_TIME 187u
#define CYW43_WLC_SET_SCAN_PASSIVE_TIME 258u
#define CYW43_WLC_SET_PM       86u
#define CYW43_WLC_GET_VAR       262u
#define CYW43_WLC_SET_VAR       263u
#define CYW43_WLC_SET_WSEC_PMK  268u

#define CYW43_EV_ESCAN_RESULT   69u
#define CYW43_STATUS_SUCCESS    0u
#define CYW43_STATUS_PARTIAL    8u
#define CYW43_ESCAN_REQ_VERSION 1u
#define CYW43_ESCAN_ACTION_START 1u
#define CYW43_ESCAN_SYNC_ID     0x1234u

#define CYW43_WL_IOVAR_BUF_LEN  1536u
#define CYW43_WL_ESCAN_PARAMS_LEN 136u
#define CYW43_WL_SCAN_RESULTS_LEN 3840u
#define CYW43_WL_MAX_SSID_LEN   32u
#define CYW43_DOT11_BSSTYPE_ANY 2u
#define CYW43_WSEC_AES          0x0004u
#define CYW43_WSEC_PASSPHRASE   0x0001u
#define CYW43_WPA_AUTH_DISABLED 0x0000u
#define CYW43_WPA_AUTH_PSK      0x0004u
#define CYW43_WPA2_AUTH_PSK     0x0080u
#define CYW43_EAPOL_KEY_TIMEOUT 5000u

// CYW43430/CYW43438 firmware RAM layout. The full Circle path discovers this
// by scanning cores; this bootstrap path uses the known Pi 3/Zero 2W value.
#define CYW43_RAM_BASE          0x00000000u
#define CYW43_RAM_SIZE          0x000C8000u
#define CYW43_RAM_SIZE_43430    0x00080000u

typedef struct {
    unsigned char enabled;
    unsigned char func1_ready;
    unsigned char func2_ready;
    unsigned char fw_loaded;
    unsigned char fw_running;
    unsigned char iface_up;
    unsigned char wifi_configured;
    unsigned char joined;
    unsigned int chip_id;
    unsigned int chip_rev;
    unsigned int arm_core;
    unsigned int arm_ctl;
    unsigned int d11_ctl;
    unsigned int socram_regs;
    unsigned int socram_ctl;
    unsigned int sd_regs;
    unsigned int ram_size;
    unsigned char mac[6];
    char country[4];
    unsigned int sdpcm_tx_seq;
    unsigned short reqid;
    unsigned char flow_mask;
    unsigned char tx_window;
    unsigned int net_tx_ok;
    unsigned int net_tx_fail;
    unsigned int net_rx_data;
    unsigned int net_rx_event;
    unsigned int net_rx_control;
    unsigned int net_rx_other;
    unsigned int last_scan_count;
    unsigned int country_rev;
    char joined_ssid[33];
} cyw43_state_t;

static cyw43_state_t g_cyw43;
static cyw43_rx_handler_t g_cyw43_rx_handler = 0;
static unsigned char* g_cyw43_clm_blob;
static unsigned int g_cyw43_clm_len;
static unsigned char g_cyw43_clm_uploaded;

typedef struct {
    unsigned short len;
    unsigned char data[CYW43_RAW_CAPTURE_MAX_BYTES];
} cyw43_raw_frame_t;

static spinlock_t g_cyw43_raw_lock;
static spinlock_t g_cyw43_raw_poll_lock;
static cyw43_raw_frame_t g_cyw43_raw_ring[CYW43_RAW_CAPTURE_DEPTH];
static unsigned int g_cyw43_raw_enabled;
static unsigned int g_cyw43_raw_head;
static unsigned int g_cyw43_raw_tail;
static unsigned int g_cyw43_raw_count;
static unsigned int g_cyw43_raw_rx_frames;
static unsigned int g_cyw43_raw_dropped;
static unsigned int g_cyw43_raw_truncated;
static unsigned int g_cyw43_raw_last_len;
static unsigned int g_cyw43_raw_last_kind;
static unsigned int g_cyw43_raw_radiotap_frames;
static unsigned int g_cyw43_raw_dot11_frames;
static unsigned int g_cyw43_raw_ethernet_frames;
static unsigned int g_cyw43_raw_unknown_frames;
static unsigned int g_cyw43_monitor_mode;
static unsigned int g_cyw43_monitor_channel;
static int g_cyw43_monitor_last_rc;
static unsigned int g_cyw43_wl_cmd_tries = 48u;
static unsigned int g_cyw43_wl_cmd_timeout_ms = 250u;

static void cyw43_drain_pending_packets(unsigned int max_frames);
static int cyw43_wl_cmd(int write, unsigned int op,
                        const unsigned char* data, unsigned int data_len,
                        unsigned char* result, unsigned int result_len,
                        unsigned int* result_actual);

static unsigned int kmin_u32(unsigned int a, unsigned int b){
    return (a < b) ? a : b;
}

static void put_le32(unsigned char out[4], unsigned int v){
    out[0] = (unsigned char)(v & 0xFFu);
    out[1] = (unsigned char)((v >> 8) & 0xFFu);
    out[2] = (unsigned char)((v >> 16) & 0xFFu);
    out[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put_le16(unsigned char out[2], unsigned int v){
    out[0] = (unsigned char)(v & 0xFFu);
    out[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static unsigned int get_le16(const unsigned char in[2]){
    return ((unsigned int)in[0]) |
           ((unsigned int)in[1] << 8);
}

static int get_le16s(const unsigned char in[2]){
    unsigned int v = get_le16(in);
    return (v & 0x8000u) ? ((int)v - 65536) : (int)v;
}

static unsigned int get_le32(const unsigned char in[4]){
    return ((unsigned int)in[0]) |
           ((unsigned int)in[1] << 8) |
           ((unsigned int)in[2] << 16) |
           ((unsigned int)in[3] << 24);
}

static unsigned int get_be32(const unsigned char in[4]){
    return ((unsigned int)in[0] << 24) |
           ((unsigned int)in[1] << 16) |
           ((unsigned int)in[2] << 8) |
           ((unsigned int)in[3]);
}

static unsigned int round4_u32(unsigned int n){
    return (n + 3u) & ~3u;
}

static void mem_zero_local(unsigned char* p, unsigned int n){
    if (!p){
        return;
    }
    for (unsigned int i = 0; i < n; i++){
        p[i] = 0;
    }
}

static void mem_copy_local(unsigned char* dst, const unsigned char* src, unsigned int n){
    if (!dst || !src){
        return;
    }
    for (unsigned int i = 0; i < n; i++){
        dst[i] = src[i];
    }
}

static int c_is_space(char c){
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static int c_hex(char c){
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int c_is_alnum(char c){
    return ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z'));
}

static int parse_u32_text(const char* s, unsigned int n, unsigned int* out){
    unsigned int v = 0;
    if (!s || !out || n == 0u){
        return -1;
    }
    for (unsigned int i = 0; i < n; i++){
        if (s[i] < '0' || s[i] > '9'){
            return -1;
        }
        v = (v * 10u) + (unsigned int)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static unsigned int strn_len_local(const char* s, unsigned int cap){
    unsigned int n = 0;
    if (!s){
        return 0;
    }
    while (n < cap && s[n]){
        n++;
    }
    return n;
}

static int str_eq_literal(const char* a, unsigned int alen, const char* lit){
    unsigned int i = 0;
    while (i < alen && lit[i]){
        if (a[i] != lit[i]){
            return 0;
        }
        i++;
    }
    return (i == alen && lit[i] == 0);
}

static int parse_mac_text(const char* s, unsigned char out[6]){
    for (unsigned int i = 0; i < 6u; i++){
        int hi = c_hex(*s++);
        int lo = c_hex(*s++);
        if (hi < 0 || lo < 0){
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
        if (i < 5u){
            if (*s != ':'){
                return -1;
            }
            s++;
        }
    }
    return 0;
}

static int nvram_pack_and_parse(const char* text, unsigned int text_len,
                                unsigned char* packed, unsigned int packed_cap,
                                unsigned int* packed_len){
    unsigned int r = 0;
    unsigned int w = 0;
    unsigned int entries = 0;
    unsigned char have_mac = 0;
    unsigned char have_country = 0;
    unsigned char have_regrev = 0;
    unsigned int parsed_regrev = 0u;

    if (!text || !packed || !packed_len || packed_cap < 4u){
        return -1;
    }

    while (r < text_len){
        unsigned int line_start = r;
        unsigned int line_end = r;
        unsigned int s = 0;
        unsigned int e = 0;
        unsigned int i = 0;
        unsigned int eq = 0xFFFFFFFFu;

        while (line_end < text_len && text[line_end] != '\n' && text[line_end] != '\r'){
            line_end++;
        }
        r = line_end;
        while (r < text_len && (text[r] == '\n' || text[r] == '\r')){
            r++;
        }

        s = line_start;
        e = line_end;
        while (s < e && c_is_space(text[s])){
            s++;
        }
        while (e > s && c_is_space(text[e - 1u])){
            e--;
        }
        if (s >= e){
            continue;
        }
        if (text[s] == '#'){
            continue;
        }

        for (i = s; i < e; i++){
            if (text[i] == '='){
                eq = i;
                break;
            }
        }
        if (eq == 0xFFFFFFFFu || eq == s || eq + 1u >= e){
            continue;
        }

        // Track selected keys for runtime status.
        if (str_eq_literal(&text[s], eq - s, "macaddr")){
            if (parse_mac_text(&text[eq + 1u], g_cyw43.mac) == 0){
                have_mac = 1;
            }
        } else if (str_eq_literal(&text[s], eq - s, "ccode") ||
                   str_eq_literal(&text[s], eq - s, "country")){
            unsigned int vlen = e - (eq + 1u);
            /*
             * Broadcom's wl_country uses four-byte strings, so preserve
             * Raspberry Pi NVRAM values such as ccode=ALL. Truncating this to
             * "AL" or replacing it with "WW" can leave BCM43430 rev-2 firmware
             * with an empty scan channel plan.
             */
            if (vlen >= 2u && vlen <= 3u){
                unsigned int ok = 1u;
                for (unsigned int j = 0; j < vlen; j++){
                    if (!c_is_alnum(text[eq + 1u + j])){
                        ok = 0u;
                    }
                }
                if (!ok){
                    continue;
                }
                for (unsigned int j = 0; j < sizeof(g_cyw43.country); j++){
                    g_cyw43.country[j] = 0;
                }
                for (unsigned int j = 0; j < vlen; j++){
                    g_cyw43.country[j] = text[eq + 1u + j];
                }
                have_country = 1;
            }
        } else if (str_eq_literal(&text[s], eq - s, "regrev")){
            if (parse_u32_text(&text[eq + 1u], e - (eq + 1u), &parsed_regrev) == 0){
                have_regrev = 1;
            }
        }

        if ((w + (e - s) + 1u) >= packed_cap){
            return -1;
        }
        for (i = s; i < e; i++){
            packed[w++] = (unsigned char)text[i];
        }
        packed[w++] = 0;
        entries++;
    }

    if (!have_mac){
        g_cyw43.mac[0] = 0x02;
        g_cyw43.mac[1] = 0x51;
        g_cyw43.mac[2] = 0x4F;
        g_cyw43.mac[3] = 0x53;
        g_cyw43.mac[4] = 0x43;
        g_cyw43.mac[5] = 0x43;
    }
    if (!have_country){
        g_cyw43.country[0] = 'W';
        g_cyw43.country[1] = 'W';
        g_cyw43.country[2] = 0;
        g_cyw43.country[3] = 0;
    }
    /*
     * Broadcom firmware treats 0 as "use the firmware/CLM default regulatory
     * revision". Passing UINT32_MAX here can leave some BCM43430 rev-2 Zero 2 W
     * firmware builds with no usable scan plan even though radio=0/up=1.
     */
    g_cyw43.country_rev = have_regrev ? parsed_regrev : 0u;

    if (entries == 0u){
        return -1;
    }

    if ((w + 1u) >= packed_cap){
        return -1;
    }
    packed[w++] = 0; // double-NUL terminator
    while (w & 3u){
        if (w >= packed_cap){
            return -1;
        }
        packed[w++] = 0;
    }
    *packed_len = w;
    return 0;
}

static int cyw43_backplane_window(unsigned int addr){
    unsigned int window = addr & ~(CYW43_SB_WINDOW_SIZE - 1u);
    if (sdio_bus_cmd52_write(1, CYW43_SB_ADDR_REG + 0u, (unsigned char)((window >> 8) & 0xFFu)) != 0){
        return -1;
    }
    if (sdio_bus_cmd52_write(1, CYW43_SB_ADDR_REG + 1u, (unsigned char)((window >> 16) & 0xFFu)) != 0){
        return -1;
    }
    if (sdio_bus_cmd52_write(1, CYW43_SB_ADDR_REG + 2u, (unsigned char)((window >> 24) & 0xFFu)) != 0){
        return -1;
    }
    return 0;
}

static int cyw43_backplane_write(unsigned int addr, const unsigned char* data, unsigned int len){
    unsigned int off = 0;
    while (off < len){
        unsigned int cur = addr + off;
        unsigned int window_left = CYW43_SB_WINDOW_SIZE - (cur & (CYW43_SB_WINDOW_SIZE - 1u));
        unsigned int n = kmin_u32(CYW43_SDIO_XFER_CHUNK, len - off);
        n = kmin_u32(n, window_left);

        if (cyw43_backplane_window(cur) != 0){
            return -1;
        }
        if (sdio_bus_cmd53_write(1, (cur & (CYW43_SB_WINDOW_SIZE - 1u)) | CYW43_SB_32BIT_ADDR,
                                 &data[off], n) != 0){
            uart_puts("CYW43: backplane write fail off=");
            uart_puthex(off);
            uart_puts(" addr=");
            uart_puthex(cur);
            uart_puts("\n");
            return -1;
        }
        off += n;
    }
    return 0;
}

static int cyw43_backplane_read(unsigned int addr, unsigned char* data, unsigned int len){
    unsigned int off = 0;
    while (off < len){
        unsigned int cur = addr + off;
        unsigned int window_left = CYW43_SB_WINDOW_SIZE - (cur & (CYW43_SB_WINDOW_SIZE - 1u));
        unsigned int n = kmin_u32(CYW43_SDIO_XFER_CHUNK, len - off);
        n = kmin_u32(n, window_left);

        if (cyw43_backplane_window(cur) != 0){
            return -1;
        }
        if (sdio_bus_cmd53_read(1, (cur & (CYW43_SB_WINDOW_SIZE - 1u)) | CYW43_SB_32BIT_ADDR,
                                &data[off], n) != 0){
            uart_puts("CYW43: backplane read fail off=");
            uart_puthex(off);
            uart_puts(" addr=");
            uart_puthex(cur);
            uart_puts("\n");
            return -1;
        }
        off += n;
    }
    return 0;
}

static int cyw43_backplane_read32(unsigned int addr, unsigned int* out){
    unsigned char b[4];
    if (!out || cyw43_backplane_read(addr, b, sizeof(b)) != 0){
        return -1;
    }
    *out = get_le32(b);
    return 0;
}

static int cyw43_backplane_write32(unsigned int addr, unsigned int val){
    unsigned char b[4];
    put_le32(b, val);
    return cyw43_backplane_write(addr, b, sizeof(b));
}

static void cyw43_delay(unsigned int n){
    while (n--){
        asm volatile("nop");
    }
}

static void cyw43_delay_ms(unsigned int ms){
    unsigned long start = system_ticks;
    unsigned long guard = (unsigned long)ms * 2000000u;
    if (ms == 0u){
        return;
    }
    /*
     * Wi-Fi commands run after the kernel timer is active. If this path is ever
     * called earlier, fall back to a conservative spin instead of hanging.
     */
    if (start != 0u || system_ticks != 0u){
        while ((unsigned long)(system_ticks - start) < (unsigned long)ms){
            asm volatile("nop");
            if (guard-- == 0u){
                break;
            }
        }
        return;
    }
    while (ms--){
        cyw43_delay(250000u);
    }
}

static int cyw43_request_alp_clock(void){
    unsigned char csr = 0;
    if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, CYW43_CLK_NO_HW_REQ | CYW43_CLK_REQ_ALP) != 0){
        return -1;
    }
    for (unsigned int i = 0; i < 20000u; i++){
        if (sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr) == 0 &&
            (csr & CYW43_CLK_ALP_AVAIL)){
            (void)sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, CYW43_CLK_NO_HW_REQ | CYW43_CLK_FORCE_ALP);
            return 0;
        }
        asm volatile("nop");
    }
    uart_puts("CYW43: ALP clock timeout csr=");
    uart_puthex(csr);
    uart_puts("\n");
    return -1;
}

static int cyw43_discover_cores(void){
    unsigned int chip = 0;
    unsigned int erom = 0;
    unsigned char buf[CYW43_CRESCAN_SIZE];

    g_cyw43.arm_core = 0;
    g_cyw43.arm_ctl = 0;
    g_cyw43.d11_ctl = 0;
    g_cyw43.socram_regs = 0;
    g_cyw43.socram_ctl = 0;
    g_cyw43.sd_regs = 0;
    g_cyw43.ram_size = CYW43_RAM_SIZE;

    if (cyw43_backplane_read32(CYW43_ENUM_BASE, &chip) != 0){
        uart_puts("CYW43: chip id read failed\n");
        return -1;
    }

    g_cyw43.chip_id = chip & 0xFFFFu;
    g_cyw43.chip_rev = (chip >> 16) & 0xFu;
    uart_puts("CYW43: chip id/rev=");
    uart_puthex(chip);
    uart_puts("\n");

    if (cyw43_backplane_read32(CYW43_ENUM_BASE + 63u * 4u, &erom) != 0 ||
        cyw43_backplane_read(erom, buf, sizeof(buf)) != 0){
        uart_puts("CYW43: core scan read failed\n");
        return -1;
    }

    unsigned int coreid = 0;
    for (unsigned int i = 0; i + 4u < sizeof(buf); i += 4u){
        unsigned int tag = buf[i] & 0xFu;

        if (tag == 0xFu){
            break;
        }
        if (tag == 0x1u){
            if (i + 7u < sizeof(buf) && ((buf[i + 4u] & 0xFu) == 0x1u)){
                coreid = ((unsigned int)buf[i + 1u] | ((unsigned int)buf[i + 2u] << 8)) & 0xFFFu;
                i += 4u;
            }
            continue;
        }
        if (tag == 0x5u){
            unsigned int addr = ((unsigned int)buf[i + 1u] << 8) |
                                ((unsigned int)buf[i + 2u] << 16) |
                                ((unsigned int)buf[i + 3u] << 24);
            addr &= ~0xFFFu;

            if (coreid == CYW43_CORE_ARM_CM3 || coreid == CYW43_CORE_ARM_CR4){
                g_cyw43.arm_core = coreid;
                if (buf[i] & 0xC0u){
                    if (g_cyw43.arm_ctl == 0u) g_cyw43.arm_ctl = addr;
                }
            } else if (coreid == CYW43_CORE_D11){
                if (buf[i] & 0xC0u){
                    g_cyw43.d11_ctl = addr;
                }
            } else if (coreid == CYW43_CORE_SOCSRAM){
                if (buf[i] & 0xC0u){
                    g_cyw43.socram_ctl = addr;
                } else if (g_cyw43.socram_regs == 0u){
                    g_cyw43.socram_regs = addr;
                }
            } else if (coreid == CYW43_CORE_SDIO){
                if ((buf[i] & 0xC0u) == 0u){
                    g_cyw43.sd_regs = addr;
                }
            }
        }
    }

    uart_puts("CYW43: cores arm=");
    uart_puthex(g_cyw43.arm_ctl);
    uart_puts(" d11=");
    uart_puthex(g_cyw43.d11_ctl);
    uart_puts(" soc=");
    uart_puthex(g_cyw43.socram_ctl);
    uart_puts("/");
    uart_puthex(g_cyw43.socram_regs);
    uart_puts(" sd=");
    uart_puthex(g_cyw43.sd_regs);
    uart_puts("\n");

    if (g_cyw43.arm_ctl == 0u || g_cyw43.d11_ctl == 0u || g_cyw43.sd_regs == 0u){
        uart_puts("CYW43: required cores missing\n");
        return -1;
    }
    if (g_cyw43.arm_core != CYW43_CORE_ARM_CM3){
        uart_puts("CYW43: only ARM CM3 firmware release is implemented\n");
        return -1;
    }
    return 0;
}

static int cyw43_core_disable(unsigned int regs, unsigned int pre, unsigned int ioctl){
    unsigned int reset = 0;
    if (cyw43_backplane_read32(regs + CYW43_CORE_RESETCTRL, &reset) != 0){
        return -1;
    }
    if (reset & 1u){
        return cyw43_backplane_write32(regs + CYW43_CORE_IOCTRL, 3u | ioctl);
    }
    if (cyw43_backplane_write32(regs + CYW43_CORE_IOCTRL, 3u | pre) != 0 ||
        cyw43_backplane_read32(regs + CYW43_CORE_IOCTRL, &reset) != 0 ||
        cyw43_backplane_write32(regs + CYW43_CORE_RESETCTRL, 1u) != 0){
        return -1;
    }
    for (unsigned int i = 0; i < 500u; i++){
        if (cyw43_backplane_read32(regs + CYW43_CORE_RESETCTRL, &reset) == 0 && (reset & 1u)){
            return cyw43_backplane_write32(regs + CYW43_CORE_IOCTRL, 3u | ioctl);
        }
        cyw43_delay(1000u);
    }
    uart_puts("CYW43: core disable timeout regs=");
    uart_puthex(regs);
    uart_puts(" reset=");
    uart_puthex(reset);
    uart_puts("\n");
    return -1;
}

static int cyw43_core_reset(unsigned int regs, unsigned int pre, unsigned int ioctl){
    unsigned int reset = 0;
    if (cyw43_core_disable(regs, pre, ioctl) != 0){
        return -1;
    }
    for (unsigned int i = 0; i < 500u; i++){
        if (cyw43_backplane_write32(regs + CYW43_CORE_RESETCTRL, 0u) != 0){
            return -1;
        }
        cyw43_delay(4000u);
        if (cyw43_backplane_read32(regs + CYW43_CORE_RESETCTRL, &reset) == 0 && !(reset & 1u)){
            return cyw43_backplane_write32(regs + CYW43_CORE_IOCTRL, 1u | ioctl);
        }
    }
    uart_puts("CYW43: core reset timeout regs=");
    uart_puthex(regs);
    uart_puts(" reset=");
    uart_puthex(reset);
    uart_puts("\n");
    return -1;
}

static int cyw43_prepare_firmware_cores(void){
    if (cyw43_core_disable(g_cyw43.arm_ctl, 0u, 0u) != 0){
        uart_puts("CYW43: ARM core disable failed\n");
        return -1;
    }
    if (cyw43_core_reset(g_cyw43.d11_ctl, 8u | 4u, 4u) != 0){
        uart_puts("CYW43: D11 core reset failed\n");
        return -1;
    }
    return 0;
}

static int cyw43_scan_socram(void){
    unsigned int coreinfo = 0;
    unsigned int banks = 0;
    unsigned int size = 0;

    if (g_cyw43.socram_ctl == 0u || g_cyw43.socram_regs == 0u){
        uart_puts("CYW43: SOCRAM core missing; using default RAM size\n");
        g_cyw43.ram_size = CYW43_RAM_SIZE;
        return 0;
    }

    if (cyw43_core_reset(g_cyw43.socram_ctl, 0u, 0u) != 0){
        uart_puts("CYW43: SOCRAM reset failed\n");
        return -1;
    }
    if (cyw43_backplane_read32(g_cyw43.socram_regs + CYW43_SOCRAM_COREINFO, &coreinfo) != 0){
        uart_puts("CYW43: SOCRAM coreinfo read failed\n");
        return -1;
    }

    banks = (coreinfo >> 4) & 0xFu;
    if (banks == 0u || banks > 16u){
        uart_puts("CYW43: bad SOCRAM bank count=");
        uart_putdec(banks);
        uart_puts(" coreinfo=");
        uart_puthex(coreinfo);
        uart_puts("\n");
        return -1;
    }

    for (unsigned int i = 0; i < banks; i++){
        unsigned int bankinfo = 0;
        if (cyw43_backplane_write32(g_cyw43.socram_regs + CYW43_SOCRAM_BANKIDX, i) != 0 ||
            cyw43_backplane_read32(g_cyw43.socram_regs + CYW43_SOCRAM_BANKINFO, &bankinfo) != 0){
            uart_puts("CYW43: SOCRAM bank read failed\n");
            return -1;
        }
        size += 8192u * ((bankinfo & 0x3Fu) + 1u);
    }

    g_cyw43.ram_size = size;
    uart_puts("CYW43: SOCRAM size=");
    uart_puthex(g_cyw43.ram_size);
    uart_puts("\n");

    // Circle powers bank 3 for 43430 after scanning RAM banks.
    if (g_cyw43.chip_id == 43430u){
        (void)cyw43_backplane_write32(g_cyw43.socram_regs + CYW43_SOCRAM_BANKIDX, 3u);
        (void)cyw43_backplane_write32(g_cyw43.socram_regs + CYW43_SOCRAM_BANKPDA, 0u);
    }
    return 0;
}

static void cyw43_select_firmware_ram_top(unsigned int fw_len){
    /*
     * The BCM43430/2 path used by Pi Zero-class boards expects the firmware
     * NVRAM token near the 512 KiB top of RAM. Some core scans report larger
     * SOCRAM layouts shared with later 4343x parts; placing the token at that
     * larger top lets writes succeed but firmware never reaches the mailbox.
     *
     * The known Zero W bare-metal flow writes NVRAM at 0x80000 - len - 4.
     * Keep larger RAM for non-43430 parts and for any future oversized image.
     */
    if (g_cyw43.chip_id == 43430u &&
        fw_len < CYW43_RAM_SIZE_43430 &&
        g_cyw43.ram_size > CYW43_RAM_SIZE_43430){
        uart_puts("CYW43: BCM43430 RAM top override ");
        uart_puthex(g_cyw43.ram_size);
        uart_puts(" -> ");
        uart_puthex(CYW43_RAM_SIZE_43430);
        uart_puts("\n");
        g_cyw43.ram_size = CYW43_RAM_SIZE_43430;
    }
}

static int cyw43_enable_ht_clock(void){
    unsigned char csr = 0;
    unsigned char force_csr = 0;

    (void)sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr);
    uart_puts("CYW43: HT pre csr=");
    uart_puthex(csr);
    uart_puts("\n");

    if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, CYW43_CLK_NO_HW_REQ | CYW43_CLK_REQ_HT) != 0){
        return -1;
    }
    uart_puts("CYW43: requesting HT clock\n");
    for (unsigned int i = 0; i < 20u; i++){
        if (sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr) != 0){
            uart_puts("CYW43: HT csr read failed i=");
            uart_putdec(i);
            uart_puts("\n");
            return -1;
        }
        if (i == 0u || i == 5u || i == 10u || i == 15u){
            uart_puts("CYW43: HT poll csr=");
            uart_puthex(csr);
            uart_puts("\n");
        }
        if (csr & CYW43_CLK_HT_AVAIL){
            if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, CYW43_CLK_NO_HW_REQ | CYW43_CLK_FORCE_HT) != 0){
                return -1;
            }
            (void)sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr);
            uart_puts("CYW43: HT forced csr=");
            uart_puthex(csr);
            uart_puts("\n");
            return 0;
        }
        cyw43_delay(250000u);
    }
    uart_puts("CYW43: HT clock timeout csr=");
    uart_puthex(csr);
    uart_puts("\n");

    /*
     * Some BCM43430/2 firmwares do not raise HT_AVAIL from REQ_HT alone after
     * the ARM core is released. Circle-style bring-up tolerates this by forcing
     * HT, then polling until the chip reports the clock as available.
     */
    force_csr = CYW43_CLK_NO_HW_REQ | CYW43_CLK_REQ_HT | CYW43_CLK_FORCE_HT;
    uart_puts("CYW43: forcing HT clock\n");
    for (unsigned int i = 0; i < 60u; i++){
        if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, force_csr) != 0){
            return -1;
        }
        cyw43_delay(100000u);
        if (sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr) != 0){
            return -1;
        }
        if (i == 0u || i == 10u || i == 20u || i == 40u){
            uart_puts("CYW43: HT force poll csr=");
            uart_puthex(csr);
            uart_puts("\n");
        }
        if (csr & CYW43_CLK_HT_AVAIL){
            if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG,
                                     CYW43_CLK_NO_HW_REQ | CYW43_CLK_FORCE_HT) != 0){
                return -1;
            }
            (void)sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr);
            uart_puts("CYW43: HT forced csr=");
            uart_puthex(csr);
            uart_puts("\n");
            return 0;
        }
    }
    uart_puts("CYW43: HT force timeout csr=");
    uart_puthex(csr);
    uart_puts("\n");
    if ((csr & CYW43_CLK_ALP_AVAIL) &&
        (csr & CYW43_CLK_FORCE_HT) &&
        (csr & CYW43_CLK_REQ_HT)){
        /*
         * BCM43430/2 on Pi Zero 2 W has been observed to stick at 0x72 here:
         * ALP available plus FORCE_HT/REQ_HT latched, but HT_AVAIL not yet
         * asserted. Do not fail before the firmware mailbox/F2 setup path has
         * a chance to run; later control IOCTLs will tell us if the chip truly
         * cannot enter HT.
         */
        uart_puts("CYW43: HT not reported; continuing with forced clock\n");
        return 0;
    }
    return -1;
}

static int cyw43_enable_ht_clock_bcm43430(void){
    unsigned char csr = 0;

    /*
     * Match Circle's ether4330 sbenable() sequence for BCM43430-family chips:
     * clear CLKCSR, wait, request HT without NO_HW_REQ, wait up to ~5 seconds,
     * then keep HT forced. The earlier 0xD2 shortcut matches some notes but
     * leaves this Pi Zero 2 W stuck before firmware can post its mailbox.
     */
    if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, 0u) != 0){
        return -1;
    }
    cyw43_delay_ms(1u);
    if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, CYW43_CLK_REQ_HT) != 0){
        return -1;
    }

    (void)sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr);
    uart_puts("CYW43: BCM43430 HT req csr=");
    uart_puthex(csr);
    uart_puts("\n");

    for (unsigned int i = 0; i < 50u; i++){
        if (sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr) != 0){
            return -1;
        }
        if (i == 0u || i == 5u || i == 10u || i == 25u || i == 49u){
            uart_puts("CYW43: BCM43430 HT poll csr=");
            uart_puthex(csr);
            uart_puts("\n");
        }
        if (csr & CYW43_CLK_HT_AVAIL){
            if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG,
                                     (unsigned char)(csr | CYW43_CLK_FORCE_HT)) != 0){
                return -1;
            }
            cyw43_delay_ms(10u);
            (void)sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr);
            uart_puts("CYW43: BCM43430 HT ready csr=");
            uart_puthex(csr);
            uart_puts("\n");
            return 0;
        }
        cyw43_delay_ms(100u);
    }

    uart_puts("CYW43: BCM43430 HT clock failed csr=");
    uart_puthex(csr);
    uart_puts("\n");
    return -1;
}

static int cyw43_enable_function2(void){
    uart_puts("CYW43: enabling function 2\n");
    if (sdio_bus_enable_func(2) != 0 ||
        sdio_bus_wait_func_ready(2, 1000) != 0){
        uart_puts("CYW43: function 2 not ready\n");
        return -1;
    }
    uart_puts("CYW43: function 2 ready\n");
    if (sdio_bus_cmd52_write(0, 0x04u, (1u << 1) | (1u << 2) | 1u) != 0){
        uart_puts("CYW43: host int enable failed\n");
        return -1;
    }
    uart_puts("CYW43: host interrupts enabled\n");
    g_cyw43.func2_ready = 1;
    return 0;
}

static void cyw43_program_f2_watermark(void){
    if (sdio_bus_cmd52_write(1, CYW43_WATERMARK_REG, CYW43_DEFAULT_F2_WM) != 0){
        uart_puts("CYW43: F2 watermark write failed\n");
    }
}

static int cyw43_sdio_keep_awake(void){
    unsigned char csr = 0;
    unsigned char want = CYW43_SLEEP_CSR_KSO | CYW43_SLEEP_CSR_DEVON;
    unsigned char saw_kso = 0;

    if (!g_cyw43.func1_ready){
        return -1;
    }

    for (unsigned int i = 0; i < 64u; i++){
        /*
         * Broadcom SDIO KSO writes are synchronized through the low-speed PMU
         * domain. Linux brcmfmac rewrites and rereads until KSO+DEVON stick.
         */
        (void)sdio_bus_cmd52_write(1, CYW43_SLEEP_CSR_REG, CYW43_SLEEP_CSR_KSO);
        cyw43_delay(30000u);
        if (sdio_bus_cmd52_read(1, CYW43_SLEEP_CSR_REG, &csr) == 0){
            if ((csr & want) == want){
                return 0;
            }
            if (csr & CYW43_SLEEP_CSR_KSO){
                saw_kso = 1;
            }
        }
        cyw43_delay(30000u);
    }

    uart_puts("CYW43: KSO wake incomplete csr=");
    uart_puthex(csr);
    uart_puts("\n");
    return saw_kso ? 0 : -1;
}

static int cyw43_wait_firmware_ready(void){
    unsigned int ints = 0;
    unsigned int mbox = 0;
    unsigned char intpend = 0;
    unsigned int proto = 0;
    uart_puts("CYW43: waiting firmware mailbox\n");
    for (unsigned int i = 0; i < 160u; i++){
        if (g_cyw43.sd_regs != 0u){
            (void)sdio_bus_cmd52_read(0, 0x05u, &intpend);
            (void)cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, &ints);
            (void)cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_HOSTMBOX_DATA, &mbox);
            if ((i % 10u) == 0u){
                uart_puts("CYW43: fw poll pend=");
                uart_puthex(intpend);
                uart_puts(" ints=");
                uart_puthex(ints);
                uart_puts(" mbox=");
                uart_puthex(mbox);
                uart_puts("\n");
            }
            if (mbox & CYW43_HMB_DATA_FWHALT){
                uart_puts("CYW43: firmware mailbox reports halt\n");
                return -1;
            }
            /*
             * Match Circle/Plan9-style mailbox handling: acknowledge the
             * ready mailbox immediately and clear the interrupt word the chip
             * actually reported. Waiting for the mailbox to go quiet can leave
             * non-host status bits (notably 0x00800000) latched and later
             * make control commands time out.
             */
            if (mbox & (CYW43_HMB_DATA_READY_MASK |
                        CYW43_SD_FW_READY |
                        CYW43_SD_FW_READY_ALT)){
                proto = (mbox & CYW43_HMB_DATA_VERSION_MASK) >> CYW43_HMB_DATA_VERSION_SHIFT;
                (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX, 2u);
                if (ints){
                    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, ints);
                }
                uart_puts("CYW43: firmware ready proto=");
                uart_putdec(proto);
                uart_puts(" mbox=");
                uart_puthex(mbox);
                uart_puts("\n");
                return 0;
            }
            if (ints){
                (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, ints);
            }
        }
        cyw43_delay(250000u);
    }
    uart_puts("CYW43: firmware ready timeout ints=");
    uart_puthex(ints);
    uart_puts(" mbox=");
    uart_puthex(mbox);
    uart_puts(" pend=");
    uart_puthex(intpend);
    uart_puts("\n");
    return -1;
}

static int cyw43_start_firmware(void){
    unsigned int arm_reset = 0;
    unsigned int arm_ioctl = 0;
    unsigned char clkcsr = 0;

    if (g_cyw43.fw_running){
        return 0;
    }
    if (!g_cyw43.fw_loaded){
        return -1;
    }
    if (g_cyw43.arm_ctl == 0u || g_cyw43.sd_regs == 0u){
        uart_puts("CYW43: missing core metadata for firmware start\n");
        return -1;
    }

    uart_puts("CYW43: releasing firmware core\n");
    if (cyw43_core_reset(g_cyw43.arm_ctl, 0u, 0u) != 0){
        uart_puts("CYW43: ARM core release failed\n");
        return -1;
    }
    (void)cyw43_backplane_read32(g_cyw43.arm_ctl + CYW43_CORE_RESETCTRL, &arm_reset);
    (void)cyw43_backplane_read32(g_cyw43.arm_ctl + CYW43_CORE_IOCTRL, &arm_ioctl);
    (void)sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &clkcsr);
    uart_puts("CYW43: ARM released reset=");
    uart_puthex(arm_reset);
    uart_puts(" ioctl=");
    uart_puthex(arm_ioctl);
    uart_puts(" clk=");
    uart_puthex(clkcsr);
    uart_puts("\n");
    if ((g_cyw43.chip_id == 43430u) ?
        (cyw43_enable_ht_clock_bcm43430() != 0) :
        (cyw43_enable_ht_clock() != 0)){
        return -1;
    }
    uart_puts("CYW43: programming SDIO boot mailbox\n");
    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, 0xFFFFFFFFu);
    if (cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX_DATA, 4u << 16) != 0){
        uart_puts("CYW43: SDIO interrupt setup failed\n");
        return -1;
    }
    uart_puts("CYW43: SDIO mailbox OK\n");

    /*
     * Match the working BCM43430 flow: after the boot mailbox write, enable
     * Function 2 and wait for IORX to report 0x06 before expecting firmware
     * control traffic. Waiting for the firmware mailbox first can leave Pi0
     * boards stuck with mbox=0 forever.
     */
    uart_puts("CYW43: requesting function 2\n");
    if (sdio_bus_enable_func(2) != 0){
        uart_puts("CYW43: function 2 enable request failed\n");
        return -1;
    }
    cyw43_program_f2_watermark();
    if (cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_MASK,
                                CYW43_SD_HOST_INT_MASK) != 0){
        uart_puts("CYW43: SDIO interrupt mask failed\n");
        return -1;
    }
    uart_puts("CYW43: SDIO intmask OK\n");
    if (sdio_bus_wait_func_ready(2, 8000) != 0){
        uart_puts("CYW43: function 2 not ready after boot mailbox\n");
        return -1;
    }
    uart_puts("CYW43: function 2 ready\n");
    if (sdio_bus_cmd52_write(0, 0x04u, (1u << 1) | (1u << 2) | 1u) != 0){
        uart_puts("CYW43: host int enable failed\n");
        return -1;
    }
    uart_puts("CYW43: host interrupts enabled\n");
    g_cyw43.func2_ready = 1;
    if (cyw43_wait_firmware_ready() != 0){
        return -1;
    }
    if (cyw43_sdio_keep_awake() != 0){
        uart_puts("CYW43: SDIO wake warning; continuing\n");
    }
    g_cyw43.fw_running = 1;
    return 0;
}

static int cyw43_attach_running_firmware(void){
    if (!g_cyw43.fw_running){
        return -1;
    }
    if (g_cyw43.sd_regs == 0u){
        uart_puts("CYW43: missing SD core metadata for firmware reattach\n");
        return -1;
    }
    if (cyw43_enable_ht_clock() != 0){
        return -1;
    }
    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, 0xFFFFFFFFu);
    if (cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX_DATA, 4u << 16) != 0){
        uart_puts("CYW43: SDIO reattach interrupt setup failed\n");
        return -1;
    }
    if (cyw43_enable_function2() != 0){
        return -1;
    }
    cyw43_program_f2_watermark();
    if (cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_MASK,
                                CYW43_SD_HOST_INT_MASK) != 0){
        uart_puts("CYW43: SDIO reattach intmask failed\n");
        return -1;
    }
    if (cyw43_sdio_keep_awake() != 0){
        uart_puts("CYW43: SDIO reattach wake warning; continuing\n");
    }
    cyw43_drain_pending_packets(64u);
    return 0;
}

static void cyw43_drop_sdio_state(void){
    g_cyw43.enabled = 0;
    g_cyw43.func1_ready = 0;
    g_cyw43.func2_ready = 0;
    g_cyw43.fw_running = 0;
    g_cyw43.iface_up = 0;
    g_cyw43.wifi_configured = 0;
    g_cyw43.joined = 0;
    g_cyw43.reqid = 0;
    g_cyw43.flow_mask = 0;
    g_cyw43.tx_window = 1;
    g_cyw43.net_tx_ok = 0;
    g_cyw43.net_tx_fail = 0;
    g_cyw43.net_rx_data = 0;
    g_cyw43.net_rx_event = 0;
    g_cyw43.net_rx_control = 0;
    g_cyw43.net_rx_other = 0;
    sdio_bus_reset_state();
    blockdev_reserve_emmc_for_wifi(0);
}

int cyw43_release_emmc_for_storage(void){
    if (!g_cyw43.enabled && !sdio_bus_is_ready()){
        blockdev_reserve_emmc_for_wifi(0);
        return 0;
    }

    uart_puts("CYW43: releasing EMMC bus for storage; WiFi disabled\n");

    /*
     * Pi 3/Zero-class boards share the Arasan EMMC/SDHCI controller between
     * SD-card storage and CYW43 SDIO. Make the WiFi NIC immediately appear
     * down so concurrent polling/sends stop touching EMMC registers.
     */
    if (g_cyw43.fw_running && g_cyw43.func2_ready && g_cyw43.iface_up){
        (void)cyw43_wl_cmd(1, CYW43_WLC_DOWN, 0, 0, 0, 0, 0);
        cyw43_drain_pending_packets(16u);
    }

    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.func2_ready = 0;
    g_cyw43.joined_ssid[0] = 0;

    net_wait_for_io_idle();
    (void)net_try_select_default_backend();

    g_cyw43.enabled = 0;
    g_cyw43.func1_ready = 0;
    g_cyw43.wifi_configured = 0;
    sdio_bus_suspend_state();
    blockdev_reserve_emmc_for_wifi(0);
    return 0;
}

int cyw43_init(void){
    int preserve_fw = g_cyw43.fw_loaded || g_cyw43.fw_running;
    unsigned char was_fw_running = g_cyw43.fw_running;
    unsigned int saved_sdpcm_tx_seq = g_cyw43.sdpcm_tx_seq;
    unsigned short saved_reqid = g_cyw43.reqid;
    unsigned char saved_flow_mask = g_cyw43.flow_mask;
    unsigned char saved_tx_window = g_cyw43.tx_window;

    if (g_cyw43.enabled){
        return 0;
    }

    if ((preserve_fw ? sdio_bus_reattach() : sdio_bus_init()) != 0){
        uart_puts("CYW43: SDIO init failed\n");
        return -1;
    }
    blockdev_reserve_emmc_for_wifi(1);

    uart_puts("CYW43: enabling SDIO function 1\n");
    if (sdio_bus_enable_func(1) == 0 &&
        sdio_bus_wait_func_ready(1, 500) == 0){
        g_cyw43.func1_ready = 1;
    } else{
        g_cyw43.func1_ready = 0;
        uart_puts("CYW43: function 1 not ready\n");
    }

    // Circle keeps Function 2 off at this early point. Function 2 is the
    // packet data path and should be enabled after the backplane/firmware path
    // is stable, not during plain wifiinit.
    g_cyw43.func2_ready = 0;

    g_cyw43.enabled = g_cyw43.func1_ready ? 1u : 0u;
    g_cyw43.fw_running = was_fw_running;
    g_cyw43.iface_up = 0;
    g_cyw43.wifi_configured = 0;
    g_cyw43.joined = 0;
    if (was_fw_running){
        g_cyw43.sdpcm_tx_seq = saved_sdpcm_tx_seq;
        g_cyw43.reqid = saved_reqid;
        g_cyw43.flow_mask = saved_flow_mask;
        g_cyw43.tx_window = saved_tx_window ? saved_tx_window : 1u;
    } else{
        g_cyw43.sdpcm_tx_seq = 0;
        g_cyw43.reqid = 0;
        g_cyw43.flow_mask = 0;
        g_cyw43.tx_window = 1;
    }
    g_cyw43.net_tx_ok = 0;
    g_cyw43.net_tx_fail = 0;
    g_cyw43.net_rx_data = 0;
    g_cyw43.net_rx_event = 0;
    g_cyw43.net_rx_control = 0;
    g_cyw43.net_rx_other = 0;
    g_cyw43.last_scan_count = 0;
    g_cyw43.joined_ssid[0] = 0;

    if (!g_cyw43.enabled){
        uart_puts("CYW43: function enable failed\n");
        return -1;
    }

    uart_puts("CYW43: SDIO F1=");
    uart_puts(g_cyw43.func1_ready ? "ready" : "down");
    uart_puts(" F2=deferred");
    uart_puts("\n");
    return 0;
}

int cyw43_upload_firmware_from_buffers(const unsigned char* fw_bin,
                                       unsigned int fw_len,
                                       const char* nvram_txt,
                                       unsigned int nvram_len){
    static unsigned char nvram_packed[CYW43_NVRAM_PACKED_MAX];
    unsigned int nvram_packed_len = 0;
    unsigned int nvram_addr = 0;
    unsigned int nvram_words = 0;
    unsigned int nvram_token = 0;
    unsigned char token_buf[4];
    unsigned char zero_buf[4] = {0, 0, 0, 0};
    unsigned char fw_tail_marker = 1u;

    if (!fw_bin || fw_len == 0u || !nvram_txt || nvram_len == 0u){
        return -1;
    }
    if (!g_cyw43.enabled){
        if (cyw43_init() != 0){
            return -1;
        }
    }
    if (!g_cyw43.func1_ready){
        return -1;
    }

    if (nvram_pack_and_parse(nvram_txt, nvram_len,
                             nvram_packed, sizeof(nvram_packed),
                             &nvram_packed_len) != 0){
        uart_puts("CYW43: bad NVRAM format\n");
        return -1;
    }

    if (cyw43_request_alp_clock() != 0){
        uart_puts("CYW43: ALP clock request failed\n");
        return -1;
    }

    if (cyw43_discover_cores() != 0){
        return -1;
    }
    if (cyw43_prepare_firmware_cores() != 0){
        return -1;
    }
    if (cyw43_scan_socram() != 0){
        return -1;
    }
    cyw43_select_firmware_ram_top(fw_len);

    uart_puts("CYW43: uploading firmware bytes=");
    uart_putdec(fw_len);
    uart_puts("\n");

    // Circle clears the last RAM word before the download, writes firmware at
    // RAM base, then places packed NVRAM near the RAM top and writes a length
    // token into the final word.
    if (cyw43_backplane_write(CYW43_RAM_BASE + g_cyw43.ram_size - 4u, zero_buf, sizeof(zero_buf)) != 0){
        uart_puts("CYW43: RAM token clear failed\n");
        return -1;
    }

    if (cyw43_backplane_write(CYW43_RAM_BASE, fw_bin, fw_len) != 0){
        uart_puts("CYW43: firmware upload failed\n");
        return -1;
    }
    /*
     * The known-good Zero W firmware loader writes one trailing byte after the
     * final partial firmware block. Without this marker, BCM43430/2 firmware
     * can be uploaded correctly but never reach the SDIO ready mailbox.
     */
    if ((fw_len + 1u) < (g_cyw43.ram_size - 4u) &&
        cyw43_backplane_write(CYW43_RAM_BASE + fw_len, &fw_tail_marker, 1u) != 0){
        uart_puts("CYW43: firmware tail marker write failed\n");
        return -1;
    }

    nvram_addr = CYW43_RAM_BASE + g_cyw43.ram_size - nvram_packed_len - 4u;
    uart_puts("CYW43: uploading NVRAM bytes=");
    uart_putdec(nvram_packed_len);
    uart_puts(" addr=");
    uart_puthex(nvram_addr);
    uart_puts("\n");

    if (cyw43_backplane_write(nvram_addr, nvram_packed, nvram_packed_len) != 0){
        uart_puts("CYW43: NVRAM upload failed\n");
        return -1;
    }

    nvram_words = nvram_packed_len / 4u;
    nvram_token = (nvram_words & 0xFFFFu) | ((~nvram_words) << 16);
    put_le32(token_buf, nvram_token);
    if (cyw43_backplane_write(CYW43_RAM_BASE + g_cyw43.ram_size - 4u, token_buf, sizeof(token_buf)) != 0){
        uart_puts("CYW43: NVRAM token write failed\n");
        return -1;
    }

    g_cyw43.fw_loaded = 1;
    g_cyw43.fw_running = 0;
    g_cyw43.iface_up = 0;
    g_cyw43.wifi_configured = 0;
    g_cyw43.joined = 0;
    uart_puts("CYW43: firmware + NVRAM staged over SDIO\n");
    return 0;
}

int cyw43_upload_firmware_from_fat(const char* fw_bin_83,
                                   const char* nvram_txt_83,
                                   const char* clm_blob_83){
    const char* fw_name = fw_bin_83 ? fw_bin_83 : "4343WIFIBIN";
    const char* nv_name = nvram_txt_83 ? nvram_txt_83 : "4343NVRMTXT";
    const char* clm_name = (clm_blob_83 && *clm_blob_83) ? clm_blob_83 : 0;
    unsigned char* fw_buf = 0;
    unsigned char* nv_buf = 0;
    unsigned char* clm_buf = 0;
    int fw_len = -1;
    int nv_len = -1;
    int clm_len = -1;
    int rc = -1;

    fw_buf = (unsigned char*)kmalloc(CYW43_FW_MAX_BYTES);
    nv_buf = (unsigned char*)kmalloc(CYW43_NVRAM_MAX_BYTES);
    if (!fw_buf || !nv_buf){
        uart_puts("CYW43: no memory for firmware buffers\n");
        goto out;
    }

    if (g_cyw43.enabled || sdio_bus_is_ready()){
        uart_puts("CYW43: wifiload must run before wifiinit; reboot first\n");
        goto out;
    }

    if (blockdev_reinit_emmc() != 0){
        uart_puts("CYW43: EMMC storage reinit failed\n");
        goto out;
    }
    if (fat32_init() != 0){
        uart_puts("CYW43: FAT init failed\n");
        goto out;
    }

    fw_len = fat32_read_file(fw_name, fw_buf, (int)CYW43_FW_MAX_BYTES);
    nv_len = fat32_read_file(nv_name, nv_buf, (int)CYW43_NVRAM_MAX_BYTES);
    if (fw_len <= 0 || nv_len <= 0){
        uart_puts("CYW43: firmware/NVRAM file read failed\n");
        goto out;
    }
    uart_puts("CYW43: firmware file bytes=");
    uart_putdec((unsigned int)fw_len);
    uart_puts(" nvram bytes=");
    uart_putdec((unsigned int)nv_len);
    uart_puts("\n");

    if (clm_name){
        clm_buf = (unsigned char*)kmalloc(CYW43_CLM_MAX_BYTES);
        if (!clm_buf){
            uart_puts("CYW43: no memory for CLM buffer\n");
            goto out;
        }
        clm_len = fat32_read_file(clm_name, clm_buf, (int)CYW43_CLM_MAX_BYTES);
        if (clm_len <= 0){
            uart_puts("CYW43: CLM file read failed\n");
            goto out;
        }
        if (g_cyw43_clm_blob){
            kfree_secure(g_cyw43_clm_blob, CYW43_CLM_MAX_BYTES);
            g_cyw43_clm_blob = 0;
            g_cyw43_clm_len = 0;
        }
        g_cyw43_clm_blob = clm_buf;
        g_cyw43_clm_len = (unsigned int)clm_len;
        g_cyw43_clm_uploaded = 0;
        clm_buf = 0;
        uart_puts("CYW43: CLM staged bytes=");
        uart_putdec(g_cyw43_clm_len);
        uart_puts("\n");
    } else{
        if (g_cyw43_clm_blob){
            kfree_secure(g_cyw43_clm_blob, CYW43_CLM_MAX_BYTES);
            g_cyw43_clm_blob = 0;
        }
        g_cyw43_clm_len = 0;
        g_cyw43_clm_uploaded = 0;
    }

    // The firmware blobs are now buffered in RAM. Reinitialize EMMC as WiFi
    // SDIO before touching the CYW43 backplane.
    cyw43_drop_sdio_state();
    rc = cyw43_upload_firmware_from_buffers(fw_buf, (unsigned int)fw_len,
                                            (const char*)nv_buf, (unsigned int)nv_len);

out:
    if (fw_buf){
        kfree_secure(fw_buf, CYW43_FW_MAX_BYTES);
    }
    if (nv_buf){
        kfree_secure(nv_buf, CYW43_NVRAM_MAX_BYTES);
    }
    if (clm_buf){
        kfree_secure(clm_buf, CYW43_CLM_MAX_BYTES);
    }
    return rc;
}

int cyw43_build_sdpcm(cyw43_sdpcm_hdr_t* hdr,
                      unsigned char channel,
                      unsigned int payload_len,
                      unsigned int seq){
    unsigned int frame_len = 0;
    if (!hdr || payload_len > 0x0FFFu){
        return -1;
    }
    frame_len = (unsigned int)sizeof(cyw43_sdpcm_hdr_t) + payload_len;
    hdr->frame_len = (unsigned short)frame_len;
    hdr->frame_len_cksum = (unsigned short)(~hdr->frame_len);
    hdr->seq = (unsigned char)(seq & 0xFFu);
    hdr->channel = channel;
    hdr->next_len = 0;
    hdr->hdr_len = (unsigned char)sizeof(cyw43_sdpcm_hdr_t);
    hdr->flow_control = 0;
    hdr->credit = 0;
    hdr->reserved = 0;
    return (int)frame_len;
}

static int cyw43_wait_rx_frame(unsigned int timeout_ms, int quiet){
    unsigned char count0 = 0;
    unsigned char count1 = 0;
    unsigned char intpend = 0;
    unsigned int ints = 0;
    unsigned int mbox = 0;
    unsigned int loops = timeout_ms ? (timeout_ms * 20u) : 1u;

    if (timeout_ms != 0u && loops < 1000u){
        loops = 1000u;
    }

    while (loops-- > 0u){
        if (sdio_bus_cmd52_read(1, CYW43_RFRAME_COUNT_REG, &count0) == 0 &&
            sdio_bus_cmd52_read(1, CYW43_RFRAME_COUNT_REG + 1u, &count1) == 0){
            if (count0 || count1){
                return 0;
            }
        }

        if (g_cyw43.sd_regs != 0u){
            (void)sdio_bus_cmd52_read(0, 0x05u, &intpend);
            if (cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, &ints) == 0){
                unsigned int ack = 0;
                ack = ints & ~CYW43_SD_INT_FRAME;
                if (ints & CYW43_SD_INT_MAILBOX){
                    (void)cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_HOSTMBOX_DATA, &mbox);
                    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX, 2u);
                    if (mbox & CYW43_HMB_DATA_FWHALT){
                        if (!quiet){
                            uart_puts("CYW43: firmware mailbox halt during RX wait\n");
                        }
                        return -1;
                    }
                }
                if (ints & CYW43_SD_INT_FRAME){
                    if (ack){
                        (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, ack);
                    }
                    return 0;
                }
                if (ack){
                    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, ack);
                }
            }
        }
        cyw43_delay(20000u);
    }

    if (!quiet){
        uart_puts("CYW43: rx wait timeout rfcnt=");
        uart_puthex(((unsigned int)count1 << 8) | count0);
        uart_puts(" pend=");
        uart_puthex(intpend);
        uart_puts(" ints=");
        uart_puthex(ints);
        uart_puts(" mbox=");
        uart_puthex(mbox);
        uart_puts("\n");
    }
    return -1;
}

static void cyw43_rx_halt_and_drain(void){
    unsigned char count0 = 0xFFu;
    unsigned char count1 = 0xFFu;

    (void)sdio_bus_cmd52_write(1, CYW43_FRAMECTL_REG, CYW43_FRAMECTL_RFHALT);
    for (unsigned int i = 0; i < 1000u; i++){
        if (sdio_bus_cmd52_read(1, CYW43_RFRAME_COUNT_REG, &count0) != 0 ||
            sdio_bus_cmd52_read(1, CYW43_RFRAME_COUNT_REG + 1u, &count1) != 0){
            break;
        }
        if (!count0 && !count1){
            break;
        }
    }
}

static int cyw43_prepare_packet_window(void){
    /*
     * Circle's ether4330 packetrw() uses Function 2 fixed-address packet I/O
     * at Enumbase. Its CMD53 helper masks addresses to 17 bits, so Enumbase
     * becomes address 0. This path is a FIFO and does not need the Function 1
     * backplane window used for register/SOCRAM access.
     */
    return 0;
}

static int cyw43_tx_has_credit(unsigned int channel){
    unsigned char seq = (unsigned char)(g_cyw43.sdpcm_tx_seq & 0xFFu);
    unsigned char delta = (unsigned char)(g_cyw43.tx_window - seq);

    if (channel >= 8u){
        return 0;
    }
    if (g_cyw43.flow_mask & (1u << channel)){
        return 0;
    }
    return (delta != 0u && delta < 128u);
}

static int cyw43_wait_tx_credit(unsigned int channel){
    for (unsigned int i = 0; i < 200000u; i++){
        if (cyw43_tx_has_credit(channel)){
            return 0;
        }
        cyw43_delay(50u);
    }
    uart_puts("CYW43: tx credit timeout ch=");
    uart_putdec(channel);
    uart_puts(" seq=");
    uart_putdec(g_cyw43.sdpcm_tx_seq & 0xFFu);
    uart_puts(" win=");
    uart_putdec(g_cyw43.tx_window);
    uart_puts(" flow=");
    uart_puthex(g_cyw43.flow_mask);
    uart_puts("\n");
    return -1;
}

static void cyw43_force_next_control_credit(void){
    /*
     * Nexmon monitor commands on some 43430/43436 firmwares are applied but do
     * not return a normal SDPCM control response. Without a response the TX
     * window never advances, so the next control command would block forever
     * behind our credit guard. Keep this scoped to monitor-only no-response
     * commands; the normal networking path still uses firmware credits.
     */
    g_cyw43.flow_mask &= (unsigned char)~(1u << CYW43_SDPCM_CH_CONTROL);
    g_cyw43.tx_window = (unsigned char)((g_cyw43.sdpcm_tx_seq + 1u) & 0xFFu);
}

static int cyw43_packet_write(const unsigned char* data, unsigned int len){
    unsigned int xfer_len = round4_u32(len);
    unsigned int channel = 0;
    if (!data || len < CYW43_SDPCM_HDR_LEN || xfer_len > CYW43_PACKET_MAX_BYTES){
        return -1;
    }
    channel = data[5] & 0x0Fu;
    if ((channel == CYW43_SDPCM_CH_CONTROL || channel == CYW43_SDPCM_CH_DATA) &&
        cyw43_wait_tx_credit(channel) != 0){
        return -1;
    }
    if (cyw43_prepare_packet_window() != 0){
        return -1;
    }
    return sdio_bus_cmd53_write_fixed(2, CYW43_PACKET_ADDR, data, xfer_len);
}

static int cyw43_wl_cmd_noresp(unsigned int op,
                               const unsigned char* data,
                               unsigned int data_len){
    static unsigned char tx[CYW43_PACKET_MAX_BYTES];
    unsigned int raw_frame_len = CYW43_SDPCM_HDR_LEN + CYW43_CDC_HDR_LEN + data_len;
    unsigned int frame_len = round4_u32(raw_frame_len);
    unsigned int cmd_off = CYW43_SDPCM_HDR_LEN;
    unsigned int payload_off = CYW43_SDPCM_HDR_LEN + CYW43_CDC_HDR_LEN;
    unsigned short reqid = 0;

    if (frame_len > CYW43_PACKET_MAX_BYTES || (!data && data_len != 0u)){
        return -1;
    }
    if (!g_cyw43.fw_running || !g_cyw43.func2_ready){
        return -1;
    }
    if (cyw43_sdio_keep_awake() != 0){
        return -1;
    }

    mem_zero_local(tx, frame_len);
    put_le16(tx + 0u, raw_frame_len);
    put_le16(tx + 2u, raw_frame_len ^ 0xFFFFu);
    tx[4] = (unsigned char)(g_cyw43.sdpcm_tx_seq & 0xFFu);
    tx[5] = CYW43_SDPCM_CH_CONTROL;
    tx[7] = CYW43_SDPCM_HDR_LEN;

    reqid = (unsigned short)(g_cyw43.reqid + 1u);
    if (reqid == 0u){
        reqid = 1u;
    }
    g_cyw43.reqid = reqid;

    put_le32(tx + cmd_off + 0u, op);
    put_le32(tx + cmd_off + 4u, data_len);
    put_le16(tx + cmd_off + 8u, 2u);
    put_le16(tx + cmd_off + 10u, reqid);
    put_le32(tx + cmd_off + 12u, 0u);
    if (data_len != 0u){
        mem_copy_local(tx + payload_off, data, data_len);
    }

    cyw43_force_next_control_credit();
    if (cyw43_packet_write(tx, raw_frame_len) != 0){
        return -1;
    }
    g_cyw43.sdpcm_tx_seq++;
    cyw43_force_next_control_credit();
    return 0;
}

static int cyw43_wl_noresp(unsigned int op){
    return cyw43_wl_cmd_noresp(op, 0, 0u);
}

static int cyw43_wl_set_int_noresp(unsigned int op, unsigned int value){
    unsigned char buf[4];
    put_le32(buf, value);
    return cyw43_wl_cmd_noresp(op, buf, sizeof(buf));
}

static unsigned int cyw43_packet_read_xfer_len(unsigned int len){
    /*
     * Circle's SDPCM path consumes packet bytes rounded only to 4 bytes.
     * Rounding larger than that can eat bytes from the next frame and
     * desynchronize command/response matching.
     */
    return round4_u32(len);
}

static int cyw43_packet_read(unsigned char* out, unsigned int out_cap,
                             unsigned int* out_len, unsigned int timeout_ms,
                             int quiet){
    unsigned int len = 0;
    unsigned int lenck = 0;
    unsigned int first_read = CYW43_SDPCM_FIRSTREAD;
    unsigned int total_xfer = 0;
    unsigned int remain_xfer = 0;

    if (!out || !out_len || out_cap < CYW43_SDPCM_HDR_LEN){
        return -1;
    }
    *out_len = 0;
    if (first_read > out_cap){
        first_read = 4u;
    }

    if (cyw43_wait_rx_frame(timeout_ms, quiet) != 0){
        return -1;
    }

    mem_zero_local(out, out_cap);
    if (cyw43_prepare_packet_window() != 0){
        return -1;
    }
    /*
     * The dongle FIFO path expects the Linux/Circle-style "first read":
     * consume enough bytes in one CMD53 to include the full SDPCM header, then
     * fetch any remaining payload. Reading only the four-byte frame tag can
     * leave the F2 FIFO in a bad partial-frame state.
     */
    if (sdio_bus_cmd53_read_fixed(2, CYW43_PACKET_ADDR, out, first_read) != 0){
        return -1;
    }

    len = get_le16(out + 0u);
    if (len == 0u){
        return 0;
    }
    lenck = get_le16(out + 2u);
    if (lenck != (len ^ 0xFFFFu) ||
        len < CYW43_SDPCM_HDR_LEN ||
        len > CYW43_PACKET_MAX_BYTES ||
        len > out_cap){
        if (!quiet){
            uart_puts("CYW43: bad SDPCM len=");
            uart_puthex(len);
            uart_puts(" lenck=");
            uart_puthex(lenck);
            uart_puts("\n");
        }
        cyw43_rx_halt_and_drain();
        return -1;
    }
    g_cyw43.flow_mask = out[8];
    if (out[9] != 0u){
        g_cyw43.tx_window = out[9];
    }

    total_xfer = cyw43_packet_read_xfer_len(len);
    if (total_xfer < first_read){
        total_xfer = first_read;
    }
    if (total_xfer > first_read){
        remain_xfer = total_xfer - first_read;
        if (first_read + remain_xfer > out_cap){
            return -1;
        }
        if (sdio_bus_cmd53_read_fixed(2, CYW43_PACKET_ADDR,
                                      out + first_read, remain_xfer) != 0){
            return -1;
        }
    }

    *out_len = len;
    return 0;
}

static void cyw43_drain_pending_packets(unsigned int max_frames){
    static unsigned char rx[CYW43_PACKET_MAX_BYTES];

    if (!g_cyw43.func2_ready){
        return;
    }

    for (unsigned int i = 0; i < max_frames; i++){
        unsigned int rx_len = 0;
        if (cyw43_packet_read(rx, sizeof(rx), &rx_len, 0u, 1) != 0){
            break;
        }
        if (rx_len == 0u){
            break;
        }
    }
}

static int cyw43_wl_cmd(int write, unsigned int op,
                        const unsigned char* data, unsigned int data_len,
                        unsigned char* result, unsigned int result_len,
                        unsigned int* result_actual){
    static unsigned char tx[CYW43_PACKET_MAX_BYTES];
    static unsigned char rx[CYW43_PACKET_MAX_BYTES];
    unsigned int transfer_payload_len = 0;
    unsigned int raw_frame_len = 0;
    unsigned int frame_len = 0;
    unsigned int cmd_off = CYW43_SDPCM_HDR_LEN;
    unsigned int payload_off = CYW43_SDPCM_HDR_LEN + CYW43_CDC_HDR_LEN;
    unsigned short reqid = 0;
    unsigned int seen_ctrl = 0;
    unsigned int seen_event = 0;
    unsigned int seen_data = 0;
    unsigned int seen_other = 0;
    unsigned int seen_ctrl_other_reqid = 0;

    if (!g_cyw43.fw_running || !g_cyw43.func2_ready){
        return -1;
    }
    if (cyw43_sdio_keep_awake() != 0){
        return -1;
    }

    transfer_payload_len = write ? (data_len + result_len) :
                                  ((data_len > result_len) ? data_len : result_len);
    raw_frame_len = CYW43_SDPCM_HDR_LEN + CYW43_CDC_HDR_LEN + transfer_payload_len;
    frame_len = round4_u32(raw_frame_len);
    if (frame_len > CYW43_PACKET_MAX_BYTES){
        return -1;
    }

    mem_zero_local(tx, sizeof(tx));
    put_le16(tx + 0u, raw_frame_len);
    put_le16(tx + 2u, raw_frame_len ^ 0xFFFFu);
    tx[4] = (unsigned char)(g_cyw43.sdpcm_tx_seq & 0xFFu);
    tx[5] = CYW43_SDPCM_CH_CONTROL;
    tx[6] = 0;
    tx[7] = CYW43_SDPCM_HDR_LEN;
    tx[8] = 0;
    tx[9] = 0;
    tx[10] = 0;
    tx[11] = 0;

    reqid = (unsigned short)(g_cyw43.reqid + 1u);
    if (reqid == 0u){
        reqid = 1u;
    }
    g_cyw43.reqid = reqid;

    put_le32(tx + cmd_off + 0u, op);
    put_le32(tx + cmd_off + 4u, transfer_payload_len);
    put_le16(tx + cmd_off + 8u, write ? 2u : 0u);
    put_le16(tx + cmd_off + 10u, reqid);
    put_le32(tx + cmd_off + 12u, 0u);

    if (data && data_len > 0u){
        mem_copy_local(tx + payload_off, data, data_len);
    }
    if (write && result && result_len > 0u){
        mem_copy_local(tx + payload_off + data_len, result, result_len);
    }

    if (cyw43_packet_write(tx, frame_len) != 0){
        return -1;
    }
    g_cyw43.sdpcm_tx_seq++;

    for (unsigned int tries = 0; tries < g_cyw43_wl_cmd_tries; tries++){
        unsigned int rx_len = 0;
        unsigned int channel = 0;
        unsigned int doffset = 0;
        unsigned int status = 0;
        unsigned int cdc_len = 0;
        unsigned int copy_len = 0;

        if (cyw43_packet_read(rx, sizeof(rx), &rx_len, g_cyw43_wl_cmd_timeout_ms, 0) != 0){
            continue;
        }
        if (rx_len == 0u){
            continue;
        }

        g_cyw43.flow_mask = rx[8];
        if (rx[9] != 0u){
            g_cyw43.tx_window = rx[9];
        }
        channel = rx[5] & 0x0Fu;
        doffset = rx[7];
        if (channel == CYW43_SDPCM_CH_CONTROL){
            seen_ctrl++;
        } else if (channel == CYW43_SDPCM_CH_EVENT){
            seen_event++;
        } else if (channel == CYW43_SDPCM_CH_DATA){
            seen_data++;
        } else{
            seen_other++;
        }
        if (channel != CYW43_SDPCM_CH_CONTROL){
            continue;
        }
        if (doffset < CYW43_SDPCM_HDR_LEN ||
            doffset + CYW43_CDC_HDR_LEN > rx_len){
            continue;
        }
        if ((unsigned short)get_le16(rx + doffset + 10u) != reqid){
            seen_ctrl_other_reqid++;
            continue;
        }

        status = get_le32(rx + doffset + 12u);
        if (status != 0u){
            uart_puts("CYW43: wl cmd status=");
            uart_puthex(status);
            uart_puts(" op=");
            uart_putdec(op);
            uart_puts("\n");
            return -1;
        }

        cdc_len = get_le32(rx + doffset + 4u);
        if (!write && result && result_len > 0u){
            if (doffset + CYW43_CDC_HDR_LEN > rx_len){
                return -1;
            }
            copy_len = rx_len - doffset - CYW43_CDC_HDR_LEN;
            if (copy_len > cdc_len){
                copy_len = cdc_len;
            }
            if (copy_len > result_len){
                copy_len = result_len;
            }
            mem_copy_local(result, rx + doffset + CYW43_CDC_HDR_LEN, copy_len);
        }
        if (result_actual){
            *result_actual = copy_len ? copy_len : cdc_len;
        }
        return 0;
    }

    uart_puts("CYW43: wl cmd timeout op=");
    uart_putdec(op);
    uart_puts(" c=");
    uart_putdec(seen_ctrl);
    uart_puts(" e=");
    uart_putdec(seen_event);
    uart_puts(" d=");
    uart_putdec(seen_data);
    uart_puts(" o=");
    uart_putdec(seen_other);
    uart_puts(" req_miss=");
    uart_putdec(seen_ctrl_other_reqid);
    uart_puts("\n");
    return -1;
}

static int cyw43_wl_get_var(const char* name, unsigned char* out,
                            unsigned int out_cap, unsigned int* out_len){
    unsigned char name_buf[64];
    unsigned int name_len = 0;

    if (!name || !out || out_cap == 0u){
        return -1;
    }
    name_len = strn_len_local(name, sizeof(name_buf) - 1u);
    if (name_len == 0u || name_len >= sizeof(name_buf)){
        return -1;
    }
    mem_zero_local(name_buf, sizeof(name_buf));
    for (unsigned int i = 0; i < name_len; i++){
        name_buf[i] = (unsigned char)name[i];
    }
    return cyw43_wl_cmd(0, CYW43_WLC_GET_VAR, name_buf, name_len + 1u,
                        out, out_cap, out_len);
}

static int cyw43_wl_set_var(const char* name, const unsigned char* data,
                            unsigned int data_len){
    static unsigned char buf[CYW43_WL_IOVAR_BUF_LEN];
    unsigned int name_len = 0;
    unsigned int total_len = 0;
    int rc = 0;

    if (!name){
        return -1;
    }
    name_len = strn_len_local(name, 63u);
    if (name_len == 0u || name_len >= 64u){
        return -1;
    }
    total_len = name_len + 1u + data_len;
    if (total_len > sizeof(buf)){
        return -1;
    }

    mem_zero_local(buf, sizeof(buf));
    for (unsigned int i = 0; i < name_len; i++){
        buf[i] = (unsigned char)name[i];
    }
    if (data && data_len > 0u){
        mem_copy_local(buf + name_len + 1u, data, data_len);
    }

    rc = cyw43_wl_cmd(1, CYW43_WLC_SET_VAR, buf, total_len, 0, 0, 0);
    if (rc != 0){
        uart_puts("CYW43: iovar set failed ");
        uart_puts(name);
        uart_puts("\n");
    }
    return rc;
}

static int cyw43_wl_set_var_u32(const char* name, unsigned int value){
    unsigned char buf[4];
    put_le32(buf, value);
    return cyw43_wl_set_var(name, buf, sizeof(buf));
}

static int cyw43_wl_set_var_u32_u32(const char* name,
                                    unsigned int value0,
                                    unsigned int value1){
    unsigned char buf[8];
    put_le32(buf + 0u, value0);
    put_le32(buf + 4u, value1);
    return cyw43_wl_set_var(name, buf, sizeof(buf));
}

static int cyw43_wl_set_int(unsigned int op, unsigned int value){
    unsigned char buf[4];
    put_le32(buf, value);
    return cyw43_wl_cmd(1, op, buf, sizeof(buf), 0, 0, 0);
}

static int cyw43_wl_get_int(unsigned int op, unsigned int* out){
    unsigned char buf[4];
    unsigned int actual = 0;

    if (!out){
        return -1;
    }
    mem_zero_local(buf, sizeof(buf));
    if (cyw43_wl_cmd(0, op, 0, 0, buf, sizeof(buf), &actual) != 0 ||
        actual < sizeof(buf)){
        return -1;
    }
    *out = get_le32(buf);
    return 0;
}

static int cyw43_control_ready(void){
    return g_cyw43.fw_running && g_cyw43.iface_up && g_cyw43.func2_ready;
}

int cyw43_ioctl_monitor(unsigned int mode, unsigned int channel){
    int rc = 0;

    /*
     * Nexmon's common monitor path uses WLC_SET_MONITOR=108. Mode 2 is the
     * practical "raw monitor/radiotap-ish" mode exposed by nexutil -m2.
     */
    if (!cyw43_control_ready()){
        g_cyw43_monitor_last_rc = -1;
        return -1;
    }
    if (mode > 3u || channel > 14u){
        g_cyw43_monitor_last_rc = -2;
        return -2;
    }

    if (mode == 0u){
        (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_MONITOR, 0u);
        (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_PROMISC, 0u);
        (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_SCANSUPPRESS, 0u);
        (void)cyw43_raw_capture_set_enabled(0u);
        g_cyw43_monitor_mode = 0u;
        g_cyw43_monitor_channel = 0u;
        g_cyw43_monitor_last_rc = 0;
        return 0;
    }

    if (channel != 0u){
        rc = cyw43_wl_set_int_noresp(CYW43_WLC_SET_CHANNEL, channel);
        if (rc != 0){
            g_cyw43_monitor_last_rc = -3;
            return -3;
        }
    }

    (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_SCANSUPPRESS, 1u);
    rc = cyw43_wl_set_int_noresp(CYW43_WLC_SET_PROMISC, 1u);
    if (rc != 0){
        (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_SCANSUPPRESS, 0u);
        g_cyw43_monitor_last_rc = -4;
        return -4;
    }

    rc = cyw43_wl_set_int_noresp(CYW43_WLC_SET_MONITOR, mode);
    if (rc != 0){
        (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_PROMISC, 0u);
        (void)cyw43_wl_set_int_noresp(CYW43_WLC_SET_SCANSUPPRESS, 0u);
        (void)cyw43_raw_capture_set_enabled(0u);
        g_cyw43_monitor_mode = 0u;
        g_cyw43_monitor_last_rc = -5;
        return -5;
    }

    (void)cyw43_raw_capture_set_enabled(1u);
    g_cyw43_monitor_mode = mode;
    g_cyw43_monitor_channel = channel;
    g_cyw43_monitor_last_rc = 0;
    return 0;
}

int cyw43_ioctl_monitor_status(cyw43_monitor_status_t* out){
    if (!out){
        return -1;
    }

    out->enabled = g_cyw43_monitor_mode ? 1u : 0u;
    out->requested_mode = g_cyw43_monitor_mode;
    out->monitor = g_cyw43_monitor_mode;
    out->promisc = g_cyw43_monitor_mode ? 1u : 0u;
    out->scansuppress = g_cyw43_monitor_mode ? 1u : 0u;
    out->channel = g_cyw43_monitor_channel;
    out->raw_enabled = g_cyw43_raw_enabled ? 1u : 0u;
    out->last_rc = g_cyw43_monitor_last_rc;
    return 0;
}

static int cyw43_upload_clm_blob(void){
    enum {
        CLM_HDR_LEN = 12,
        CLM_CHUNK = 1400,
        CLM_TYPE = 2,
        CLM_FLAG_CLM = 1 << 12,
        CLM_FLAG_FIRST = 1 << 1,
        CLM_FLAG_LAST = 1 << 2
    };
    static unsigned char packet[CLM_HDR_LEN + CLM_CHUNK + 8];
    unsigned int off = 0;
    unsigned int flag = CLM_FLAG_CLM | CLM_FLAG_FIRST;

    if (!g_cyw43_clm_blob || g_cyw43_clm_len == 0u || g_cyw43_clm_uploaded){
        return 0;
    }

    uart_puts("CYW43: loading CLM bytes=");
    uart_putdec(g_cyw43_clm_len);
    uart_puts("\n");

    while (off < g_cyw43_clm_len){
        unsigned int n = g_cyw43_clm_len - off;
        if (n > CLM_CHUNK){
            n = CLM_CHUNK;
        } else{
            flag |= CLM_FLAG_LAST;
        }

        mem_zero_local(packet, sizeof(packet));
        put_le16(packet + 0u, flag);
        put_le16(packet + 2u, CLM_TYPE);
        put_le32(packet + 4u, n);
        put_le32(packet + 8u, 0u);
        mem_copy_local(packet + CLM_HDR_LEN, g_cyw43_clm_blob + off, n);
        while (n & 7u){
            packet[CLM_HDR_LEN + n] = 0u;
            n++;
        }

        if (cyw43_wl_set_var("clmload", packet, CLM_HDR_LEN + n) != 0){
            uart_puts("CYW43: CLM upload failed off=");
            uart_putdec(off);
            uart_puts("\n");
            return -1;
        }

        off += (g_cyw43_clm_len - off > CLM_CHUNK) ? CLM_CHUNK : (g_cyw43_clm_len - off);
        flag &= ~CLM_FLAG_FIRST;
    }

    g_cyw43_clm_uploaded = 1;
    uart_puts("CYW43: CLM loaded\n");
    kfree_secure(g_cyw43_clm_blob, CYW43_CLM_MAX_BYTES);
    g_cyw43_clm_blob = 0;
    g_cyw43_clm_len = 0;
    return 0;
}

static int cyw43_refresh_cur_etheraddr(void){
    unsigned char mac[8];
    unsigned int actual = 0;

    mem_zero_local(mac, sizeof(mac));
    if (cyw43_wl_get_var("cur_etheraddr", mac, sizeof(mac), &actual) != 0 ||
        actual < 6u ||
        !(mac[0] || mac[1] || mac[2] || mac[3] || mac[4] || mac[5])){
        return -1;
    }

    for (unsigned int i = 0; i < 6u; i++){
        g_cyw43.mac[i] = mac[i];
    }
    return 0;
}

static void cyw43_raw_capture_reset_locked(void){
    g_cyw43_raw_head = 0;
    g_cyw43_raw_tail = 0;
    g_cyw43_raw_count = 0;
    g_cyw43_raw_rx_frames = 0;
    g_cyw43_raw_dropped = 0;
    g_cyw43_raw_truncated = 0;
    g_cyw43_raw_last_len = 0;
    g_cyw43_raw_last_kind = 0;
    g_cyw43_raw_radiotap_frames = 0;
    g_cyw43_raw_dot11_frames = 0;
    g_cyw43_raw_ethernet_frames = 0;
    g_cyw43_raw_unknown_frames = 0;
}

static unsigned int cyw43_raw_classify(const unsigned char* frame, unsigned int len){
    unsigned int rt_len = 0;
    unsigned int fc = 0;
    unsigned int eth_type = 0;

    if (!frame || len < 2u){
        return 0u;
    }

    // Radiotap starts with version=0, pad=0, little-endian header length.
    if (len >= 8u && frame[0] == 0u && frame[1] == 0u){
        rt_len = get_le16(frame + 2u);
        if (rt_len >= 8u && rt_len <= len){
            return 1u;
        }
    }

    if (len >= 14u){
        eth_type = ((unsigned int)frame[12] << 8) | (unsigned int)frame[13];
        if (eth_type >= 0x0600u && !g_cyw43_monitor_mode){
            return 3u;
        }
    }

    // 802.11 frame-control version bits should be zero, type is 0..3.
    fc = get_le16(frame);
    if ((fc & 0x0003u) == 0u && ((fc >> 2) & 0x3u) <= 3u){
        return 2u;
    }

    if (eth_type >= 0x0600u){
        return 3u;
    }

    return 0u;
}

static void cyw43_raw_capture_push(const unsigned char* frame, unsigned int len){
    unsigned int copy_len = len;
    unsigned int kind = 0;
    unsigned long irq = 0;

    if (!frame || len == 0u || !g_cyw43_raw_enabled){
        return;
    }
    if (copy_len > CYW43_RAW_CAPTURE_MAX_BYTES){
        copy_len = CYW43_RAW_CAPTURE_MAX_BYTES;
    }
    kind = cyw43_raw_classify(frame, len);

    irq = spin_lock_irqsave(&g_cyw43_raw_lock);
    if (!g_cyw43_raw_enabled){
        spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
        return;
    }
    if (g_cyw43_raw_count >= CYW43_RAW_CAPTURE_DEPTH){
        g_cyw43_raw_tail = (g_cyw43_raw_tail + 1u) % CYW43_RAW_CAPTURE_DEPTH;
        g_cyw43_raw_count--;
        g_cyw43_raw_dropped++;
    }
    g_cyw43_raw_ring[g_cyw43_raw_head].len = (unsigned short)copy_len;
    mem_copy_local(g_cyw43_raw_ring[g_cyw43_raw_head].data, frame, copy_len);
    g_cyw43_raw_head = (g_cyw43_raw_head + 1u) % CYW43_RAW_CAPTURE_DEPTH;
    g_cyw43_raw_count++;
    g_cyw43_raw_rx_frames++;
    g_cyw43_raw_last_len = len;
    g_cyw43_raw_last_kind = kind;
    if (kind == 1u){
        g_cyw43_raw_radiotap_frames++;
    } else if (kind == 2u){
        g_cyw43_raw_dot11_frames++;
    } else if (kind == 3u){
        g_cyw43_raw_ethernet_frames++;
    } else{
        g_cyw43_raw_unknown_frames++;
    }
    if (copy_len != len){
        g_cyw43_raw_truncated++;
    }
    spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
}

int cyw43_raw_capture_set_enabled(unsigned int enabled){
    unsigned long irq = spin_lock_irqsave(&g_cyw43_raw_lock);
    g_cyw43_raw_enabled = enabled ? 1u : 0u;
    cyw43_raw_capture_reset_locked();
    spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
    return 0;
}

int cyw43_raw_capture_is_enabled(void){
    unsigned int enabled = 0;
    unsigned long irq = spin_lock_irqsave(&g_cyw43_raw_lock);
    enabled = g_cyw43_raw_enabled;
    spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
    return enabled ? 1 : 0;
}

int cyw43_raw_capture_recv(unsigned char* out, unsigned int out_cap){
    unsigned int n = 0;
    unsigned long irq = 0;

    if (!out || out_cap == 0u){
        return -1;
    }

    irq = spin_lock_irqsave(&g_cyw43_raw_lock);
    if (g_cyw43_raw_count == 0u){
        spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
        return 0;
    }

    n = g_cyw43_raw_ring[g_cyw43_raw_tail].len;
    if (n > out_cap){
        n = out_cap;
    }
    mem_copy_local(out, g_cyw43_raw_ring[g_cyw43_raw_tail].data, n);
    g_cyw43_raw_tail = (g_cyw43_raw_tail + 1u) % CYW43_RAW_CAPTURE_DEPTH;
    g_cyw43_raw_count--;
    spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
    return (int)n;
}

int cyw43_raw_capture_get_status(cyw43_raw_capture_status_t* out){
    unsigned long irq = 0;

    if (!out){
        return -1;
    }
    irq = spin_lock_irqsave(&g_cyw43_raw_lock);
    out->enabled = g_cyw43_raw_enabled;
    out->queued = g_cyw43_raw_count;
    out->rx_frames = g_cyw43_raw_rx_frames;
    out->dropped = g_cyw43_raw_dropped;
    out->truncated = g_cyw43_raw_truncated;
    out->last_len = g_cyw43_raw_last_len;
    out->last_kind = g_cyw43_raw_last_kind;
    out->radiotap_frames = g_cyw43_raw_radiotap_frames;
    out->dot11_frames = g_cyw43_raw_dot11_frames;
    out->ethernet_frames = g_cyw43_raw_ethernet_frames;
    out->unknown_frames = g_cyw43_raw_unknown_frames;
    spin_unlock_irqrestore(&g_cyw43_raw_lock, irq);
    return 0;
}

static int cyw43_wl_set_ssid_cmd(unsigned int op, const char* ssid){
    unsigned char buf[36];
    unsigned int len = 0;

    mem_zero_local(buf, sizeof(buf));
    if (ssid){
        len = strn_len_local(ssid, CYW43_WL_MAX_SSID_LEN);
    }
    put_le32(buf, len);
    for (unsigned int i = 0; i < len; i++){
        buf[4u + i] = (unsigned char)ssid[i];
    }
    return cyw43_wl_cmd(1, op, buf, sizeof(buf), 0, 0, 0);
}

static int cyw43_wl_escan_submit_variant(const char* ssid, unsigned int scan_format){
    unsigned char params[CYW43_WL_ESCAN_PARAMS_LEN];
    unsigned int ssid_len = 0;
    unsigned int params_len = 72u;
    static const unsigned char chanspecs[11u * 2u] = {
        0x01u, 0x2Bu, 0x02u, 0x2Bu, 0x03u, 0x2Bu, 0x04u, 0x2Bu,
        0x05u, 0x2Bu, 0x06u, 0x2Bu, 0x07u, 0x2Bu,
        0x08u, 0x2Bu, 0x09u, 0x2Bu, 0x0Au, 0x2Bu, 0x0Bu, 0x2Bu,
    };
    /*
     * Circle's ether4330 request includes channels 1..14 and nssids=1 with a
     * trailing SSID slot. The BCM43430 rev-2 / 43436 firmware used by some
     * Zero 2 W boards appears to need this exact shape to see nearby APs.
     */
    static const unsigned char circle_chanspecs[14u * 2u] = {
        0x01u, 0x2Bu, 0x02u, 0x2Bu, 0x03u, 0x2Bu, 0x04u, 0x2Bu,
        0x05u, 0x2Eu, 0x06u, 0x2Eu, 0x07u, 0x2Eu,
        0x08u, 0x2Bu, 0x09u, 0x2Bu, 0x0Au, 0x2Bu, 0x0Bu, 0x2Bu,
        0x0Cu, 0x2Bu, 0x0Du, 0x2Bu, 0x0Eu, 0x2Bu,
    };

    mem_zero_local(params, sizeof(params));
    /*
     * Match the common bare-metal ether4330/Zerowi layout:
     * version/action/sync_id + wl_scan_params. A directed SSID goes in the
     * fixed ssidlen/ssid field at the start of wl_scan_params; the appended
     * nssids list is left empty because this firmware rejects that variant.
     * The CYW4343x firmware is picky here; malformed params return BCME_BADARG.
     */
    put_le32(params + 0u, CYW43_ESCAN_REQ_VERSION);
    put_le16(params + 4u, CYW43_ESCAN_ACTION_START);
    put_le16(params + 6u, CYW43_ESCAN_SYNC_ID);

    if (ssid && *ssid){
        ssid_len = strn_len_local(ssid, CYW43_WL_MAX_SSID_LEN);
        put_le32(params + 8u, ssid_len);
        for (unsigned int i = 0; i < ssid_len; i++){
            params[12u + i] = (unsigned char)ssid[i];
        }
    }
    for (unsigned int i = 44u; i < 50u; i++){
        params[i] = 0xFFu;
    }
    params[50u] = CYW43_DOT11_BSSTYPE_ANY;
    params[51u] = 0u;
    put_le32(params + 52u, 0xFFFFFFFFu);
    put_le32(params + 56u, 0xFFFFFFFFu);
    put_le32(params + 60u, 0xFFFFFFFFu);
    put_le32(params + 64u, 0xFFFFFFFFu);
    if (scan_format == 0u){
        /*
         * Let the firmware/CLM regulatory table choose the channel list. This
         * is the safest path on Zero 2 W BCM43430 rev-2 modules, where bad
         * local chanspecs can produce a clean scan-complete event with no APs.
         */
        put_le16(params + 68u, 0u);
        put_le16(params + 70u, 0u);
        params_len = 72u;
    } else if (scan_format == 2u){
        put_le16(params + 68u, 14u);
        put_le16(params + 70u, 1u);
        mem_copy_local(params + 72u, circle_chanspecs, sizeof(circle_chanspecs));
        params_len = 72u + sizeof(circle_chanspecs) + 36u;
        if (ssid && *ssid){
            put_le32(params + 100u, ssid_len);
            for (unsigned int i = 0; i < ssid_len; i++){
                params[104u + i] = (unsigned char)ssid[i];
            }
        }
    } else{
        put_le16(params + 68u, 11u);
        put_le16(params + 70u, 0u);
        mem_copy_local(params + 72u, chanspecs, sizeof(chanspecs));
        params_len = 72u + sizeof(chanspecs);
    }

    (void)cyw43_wl_set_int(CYW43_WLC_SET_PASSIVE_SCAN, 0u);
    return cyw43_wl_set_var("escan", params, params_len);
}

static int cyw43_wl_escan_submit(const char* ssid, unsigned int attempt){
    unsigned int prefer_circle = (g_cyw43.chip_id == 43430u && g_cyw43.chip_rev >= 2u);
    if (prefer_circle){
        if (attempt == 0u){
            return cyw43_wl_escan_submit_variant(ssid, 0u);
        }
        if (attempt == 1u){
            return cyw43_wl_escan_submit_variant(ssid, 2u);
        }
        return cyw43_wl_escan_submit_variant(ssid, 1u);
    }
    if (attempt == 0u){
        return cyw43_wl_escan_submit_variant(ssid, 1u);
    }
    if (attempt == 1u){
        return cyw43_wl_escan_submit_variant(ssid, 0u);
    }
    return cyw43_wl_escan_submit_variant(ssid, 2u);
}

static int cyw43_wl_legacy_scan_submit(const char* ssid){
    unsigned char params[64];
    unsigned int ssid_len = 0;

    mem_zero_local(params, sizeof(params));
    if (ssid && *ssid){
        ssid_len = strn_len_local(ssid, CYW43_WL_MAX_SSID_LEN);
        put_le32(params + 0u, ssid_len);
        for (unsigned int i = 0; i < ssid_len; i++){
            params[4u + i] = (unsigned char)ssid[i];
        }
    }

    for (unsigned int i = 36u; i < 42u; i++){
        params[i] = 0xFFu;
    }
    params[42u] = CYW43_DOT11_BSSTYPE_ANY;
    params[43u] = 0u;
    put_le32(params + 44u, 0xFFFFFFFFu);
    put_le32(params + 48u, 0xFFFFFFFFu);
    put_le32(params + 52u, 0xFFFFFFFFu);
    put_le32(params + 56u, 0xFFFFFFFFu);
    put_le32(params + 60u, 0u);

    (void)cyw43_wl_set_int(CYW43_WLC_SET_PASSIVE_SCAN, 0u);
    return cyw43_wl_cmd(1, CYW43_WLC_SCAN, params, sizeof(params), 0, 0, 0);
}

static int cyw43_wl_set_pmk(const char* password){
    unsigned char pmk[68];
    unsigned int len = 0;

    if (!password){
        return -1;
    }
    len = strn_len_local(password, 65u);
    if (len < 8u || len > 64u){
        uart_puts("CYW43: WPA password must be 8..64 chars\n");
        return -1;
    }

    mem_zero_local(pmk, sizeof(pmk));
    put_le16(pmk + 0u, len);
    /*
     * Follow known-good CYW43 station join paths: always mark this as a
     * passphrase payload when using WLC_SET_WSEC_PMK.
     */
    put_le16(pmk + 2u, CYW43_WSEC_PASSPHRASE);
    for (unsigned int i = 0; i < len; i++){
        pmk[4u + i] = (unsigned char)password[i];
    }

    return cyw43_wl_cmd(1, CYW43_WLC_SET_WSEC_PMK, pmk, sizeof(pmk), 0, 0, 0);
}

static int cyw43_wl_prepare_sta_supplicant(void){
    int rc = 0;

    /*
     * Match the ordering used by working CYW43 stacks before PMK programming:
     * bsscfg:sup_wpa, bsscfg:sup_wpa2_eapver=-1, bsscfg:sup_wpa_tmo=5000.
     * Some firmware builds expose only one naming variant, so try both.
     */
    if (cyw43_wl_set_var_u32_u32("bsscfg:sup_wpa", 0u, 1u) != 0 &&
        cyw43_wl_set_var_u32("sup_wpa", 1u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32_u32("bsscfg:sup_wpa2_eapver", 0u, 0xFFFFFFFFu) != 0 &&
        cyw43_wl_set_var_u32("sup_wpa2_eapver", 0xFFFFFFFFu) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32_u32("bsscfg:sup_wpa_tmo", 0u, CYW43_EAPOL_KEY_TIMEOUT) != 0 &&
        cyw43_wl_set_var_u32("sup_wpa_tmo", CYW43_EAPOL_KEY_TIMEOUT) != 0){
        rc = -1;
    }

    if (rc != 0){
        uart_puts("CYW43: supplicant iovar setup partial; continuing\n");
    }
    return 0;
}

static void cyw43_event_mask_set(unsigned char* mask, unsigned int event_id){
    mask[event_id >> 3u] |= (unsigned char)(1u << (event_id & 7u));
}

static int cyw43_wl_set_country(void){
    unsigned char country[12];
    char cc0 = g_cyw43.country[0] ? g_cyw43.country[0] : 'W';
    char cc1 = g_cyw43.country[1] ? g_cyw43.country[1] : 'W';
    char cc2 = g_cyw43.country[2];
    int rc = 0;

    mem_zero_local(country, sizeof(country));
    country[0] = (unsigned char)cc0;
    country[1] = (unsigned char)cc1;
    country[2] = (unsigned char)cc2;
    put_le32(country + 4u, g_cyw43.country_rev);
    country[8] = (unsigned char)cc0;
    country[9] = (unsigned char)cc1;
    country[10] = (unsigned char)cc2;
    uart_puts("CYW43: country ");
    uart_puts(g_cyw43.country[0] ? g_cyw43.country : "??");
    uart_puts(" rev=");
    uart_putdec(g_cyw43.country_rev);
    uart_puts("\n");

    rc = cyw43_wl_set_var("country", country, sizeof(country));
    if (rc == 0){
        return 0;
    }
    return cyw43_wl_cmd(1, CYW43_WLC_SET_COUNTRY,
                        country, sizeof(country), 0, 0, 0);
}

static int cyw43_wl_set_event_msgs(void){
    unsigned char mask[32];
    unsigned char bsscfg_mask[36];

    mem_zero_local(mask, sizeof(mask));
    cyw43_event_mask_set(mask, 0u);   // SET_SSID
    cyw43_event_mask_set(mask, 1u);   // JOIN
    cyw43_event_mask_set(mask, 3u);   // AUTH
    cyw43_event_mask_set(mask, 4u);   // AUTH_IND
    cyw43_event_mask_set(mask, 5u);   // DEAUTH
    cyw43_event_mask_set(mask, 6u);   // DEAUTH_IND
    cyw43_event_mask_set(mask, 7u);   // ASSOC
    cyw43_event_mask_set(mask, 8u);   // ASSOC_IND
    cyw43_event_mask_set(mask, 11u);  // DISASSOC
    cyw43_event_mask_set(mask, 16u);  // LINK
    cyw43_event_mask_set(mask, 26u);  // SCAN_COMPLETE
    cyw43_event_mask_set(mask, 69u);  // ESCAN_RESULT

    mem_zero_local(bsscfg_mask, sizeof(bsscfg_mask));
    put_le32(bsscfg_mask + 0u, 0u);
    mem_copy_local(bsscfg_mask + 4u, mask, sizeof(mask));
    if (cyw43_wl_set_var("bsscfg:event_msgs", bsscfg_mask, sizeof(bsscfg_mask)) == 0){
        return 0;
    }
    return cyw43_wl_set_var("event_msgs", mask, sizeof(mask));
}

static void cyw43_log_radio_status(const char* tag){
    unsigned int radio = 0;
    unsigned int up = 0;

    uart_puts("CYW43: radio status ");
    uart_puts(tag ? tag : "");
    uart_puts(" radio=");
    if (cyw43_wl_get_int(CYW43_WLC_GET_RADIO, &radio) == 0){
        uart_puthex(radio);
    } else{
        uart_puts("?");
    }
    uart_puts(" up=");
    if (cyw43_wl_get_int(CYW43_WLC_GET_UP, &up) == 0){
        uart_puthex(up);
    } else{
        uart_puts("?");
    }
    uart_puts("\n");
}

static void cyw43_clear_radio_disable_flags(void){
    unsigned int radio = 0;

    if (cyw43_wl_get_int(CYW43_WLC_GET_RADIO, &radio) != 0){
        return;
    }
    if (radio == 0u){
        return;
    }

    /*
     * Not every firmware accepts WLC_SET_RADIO, but if GET_RADIO reports
     * disable flags, attempt to clear them. A failure here is diagnostic only;
     * the later GET_RADIO line is authoritative.
     */
    if (cyw43_wl_set_int(CYW43_WLC_SET_RADIO, 0u) != 0){
        uart_puts("CYW43: radio disable flags remain=");
        uart_puthex(radio);
        uart_puts("\n");
    }
}

static int cyw43_wait_assoc(unsigned int timeout_ms){
    unsigned char bssid[8];
    unsigned int actual = 0;
    unsigned int loops = (timeout_ms / 100u) + 1u;

    if (loops > 50u){
        loops = 50u;
    }

    for (unsigned int i = 0; i < loops; i++){
        mem_zero_local(bssid, sizeof(bssid));
        actual = 0;
        if (cyw43_wl_get_var("bssid", bssid, sizeof(bssid), &actual) == 0){
            if (actual >= 6u &&
                (bssid[0] || bssid[1] || bssid[2] || bssid[3] || bssid[4] || bssid[5])){
                return 0;
            }
        }
        cyw43_delay(150000u);
    }
    return -1;
}

static int cyw43_wifi_configure_on(void){
    int rc = 0;

    /*
     * This mirrors the important "wifi on" setup used by mature CYW43 stacks:
     * set regulatory country/event delivery and disable aggregation knobs that
     * make early bare-metal SDIO bring-up much harder to debug.
     */
    if (cyw43_wl_set_country() != 0){
        rc = -1;
    }

    if (cyw43_wl_set_int(CYW43_WLC_SET_ANTDIV, 3u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32("bus:txglom", 0u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32("apsta", 1u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32("ampdu_ba_wsize", 8u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32("ampdu_mpdu", 4u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32("ampdu_rx_factor", 0u) != 0){
        rc = -1;
    }

    if (cyw43_wl_set_event_msgs() != 0){
        rc = -1;
    }
    // Circle-style scan dwell defaults that improve escan behavior on 4343x.
    if (cyw43_wl_set_int(CYW43_WLC_SET_SCAN_CHANNEL_TIME, 0x28u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_int(CYW43_WLC_SET_SCAN_UNASSOC_TIME, 0x28u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_int(CYW43_WLC_SET_SCAN_PASSIVE_TIME, 0x82u) != 0){
        rc = -1;
    }
    if (cyw43_wl_set_var_u32("roam_off", 1u) != 0){
        rc = -1;
    }
    if (rc != 0){
        uart_puts("CYW43: configure-on partial; continuing\n");
    }
    return 0;
}

static int cyw43_add_bss_info_result(const unsigned char* bss,
                                     unsigned int bss_len,
                                     cyw43_scan_result_t* out,
                                     unsigned int cap,
                                     unsigned int* count){
    unsigned int ssid_len = 0;
    unsigned int capability = 0;
    unsigned int chanspec = 0;
    int rssi_a = 0;
    int rssi_b = 0;

    if (!bss || !out || !count || *count >= cap || bss_len < 82u){
        return -1;
    }

    // wl_bss_info.length starts at offset 4 and includes the full BSS record.
    {
        unsigned int adv_len = get_le32(bss + 4u);
        if (adv_len >= 82u && adv_len < bss_len){
            bss_len = adv_len;
        }
    }

    ssid_len = (unsigned int)bss[18u];
    if (ssid_len > CYW43_WL_MAX_SSID_LEN){
        ssid_len = CYW43_WL_MAX_SSID_LEN;
    }
    if (19u + ssid_len > bss_len){
        if (bss_len > 19u){
            ssid_len = bss_len - 19u;
        } else{
            ssid_len = 0u;
        }
    }
    for (unsigned int i = 0; i < ssid_len; i++){
        char c = (char)bss[19u + i];
        out[*count].ssid[i] = (c >= 32 && c <= 126) ? c : '?';
    }
    out[*count].ssid[ssid_len] = 0;
    if (ssid_len == 0u){
        out[*count].ssid[0] = '<';
        out[*count].ssid[1] = 'h';
        out[*count].ssid[2] = 'i';
        out[*count].ssid[3] = 'd';
        out[*count].ssid[4] = 'd';
        out[*count].ssid[5] = 'e';
        out[*count].ssid[6] = 'n';
        out[*count].ssid[7] = '>';
        out[*count].ssid[8] = 0;
    }

    chanspec = get_le16(bss + 72u);
    out[*count].channel = (unsigned char)(chanspec & 0xFFu);
    capability = get_le16(bss + 16u);
    out[*count].auth = (capability & 0x0010u) ? 1u : 0u;

    /*
     * Most CYW4343x firmwares place RSSI at offset 78 (Circle path). Some
     * variants insert one extra byte before rateset and shift by +1.
     */
    rssi_a = get_le16s(bss + 78u);
    rssi_b = (bss_len >= 83u) ? get_le16s(bss + 79u) : rssi_a;
    if (rssi_a <= 0 && rssi_a >= -127){
        out[*count].rssi_dbm = rssi_a;
    } else{
        out[*count].rssi_dbm = rssi_b;
    }

    (*count)++;
    return 0;
}

static int cyw43_add_scan_event_result(const unsigned char* ev_scan,
                                       unsigned int ev_scan_len,
                                       cyw43_scan_result_t* out,
                                       unsigned int cap,
                                       unsigned int* count){
    const unsigned char* bss = 0;
    unsigned int bss_len = 0;

    if (!ev_scan || !out || !count || *count >= cap || ev_scan_len < 84u){
        return -1;
    }

    /*
     * WLC_E_ESCAN_RESULT payload is wl_escan_result_t:
     *   buflen/version/sync_id/bss_count + wl_bss_info
     * Circle's parser reads SSID/chanspec/RSSI from wl_bss_info offsets.
     */
    if (ev_scan_len >= 12u && get_le16(ev_scan + 10u) >= 1u){
        bss = ev_scan + 12u;
        bss_len = ev_scan_len - 12u;
    } else{
        // Fallback for firmwares that may already point at wl_bss_info.
        bss = ev_scan;
        bss_len = ev_scan_len;
    }

    return cyw43_add_bss_info_result(bss, bss_len, out, cap, count);
}

static int cyw43_collect_scanresults(cyw43_scan_result_t* out,
                                     unsigned int cap,
                                     unsigned int* count){
    static unsigned char results[CYW43_WL_SCAN_RESULTS_LEN];
    unsigned int actual = 0;
    unsigned int reported_count = 0;
    unsigned int off = 12u;

    if (!out || !count || cap == 0u){
        return -1;
    }

    mem_zero_local(results, sizeof(results));
    put_le32(results + 0u, sizeof(results));

    if (cyw43_wl_cmd(0, CYW43_WLC_SCAN_RESULTS,
                     results, 4u,
                     results, sizeof(results),
                     &actual) != 0){
        return -1;
    }

    if (actual < 12u){
        uart_puts("CYW43: scanresults short actual=");
        uart_putdec(actual);
        uart_puts("\n");
        return -1;
    }

    reported_count = get_le32(results + 8u);
    uart_puts("CYW43: scanresults count=");
    uart_putdec(reported_count);
    uart_puts(" actual=");
    uart_putdec(actual);
    uart_puts("\n");

    for (unsigned int i = 0; i < reported_count && *count < cap; i++){
        unsigned int bss_len = 0;
        if (off + 8u > actual){
            break;
        }
        bss_len = get_le32(results + off + 4u);
        if (bss_len < 82u || off + bss_len > actual){
            uart_puts("CYW43: scanresults bad bss len=");
            uart_putdec(bss_len);
            uart_puts(" off=");
            uart_putdec(off);
            uart_puts("\n");
            break;
        }
        (void)cyw43_add_bss_info_result(results + off, bss_len, out, cap, count);
        off += round4_u32(bss_len);
    }

    return (*count > 0u) ? 0 : -1;
}

static int cyw43_find_escan_event_msg(const unsigned char* packet,
                                      unsigned int packet_len,
                                      unsigned int* event_msg_off){
    if (!packet || !event_msg_off){
        return -1;
    }

    if (packet_len >= 72u &&
        get_be32(packet + 28u) == CYW43_EV_ESCAN_RESULT){
        *event_msg_off = 24u;
        return 0;
    }

    for (unsigned int off = 0; off + 72u <= packet_len && off < 160u; off++){
        unsigned int event_type = get_be32(packet + off + 4u);
        unsigned int status = get_be32(packet + off + 8u);
        if (event_type == CYW43_EV_ESCAN_RESULT &&
            (status == CYW43_STATUS_PARTIAL || status == CYW43_STATUS_SUCCESS)){
            *event_msg_off = off;
            return 0;
        }
    }
    return -1;
}

static void cyw43_log_control_status(const unsigned char* frame,
                                     unsigned int frame_len){
    unsigned int channel = 0;
    unsigned int doffset = 0;
    unsigned int status = 0;

    if (!frame || frame_len < CYW43_SDPCM_HDR_LEN){
        return;
    }
    channel = frame[5] & 0x0Fu;
    if (channel != CYW43_SDPCM_CH_CONTROL){
        return;
    }
    doffset = frame[7];
    if (doffset < CYW43_SDPCM_HDR_LEN ||
        doffset + CYW43_CDC_HDR_LEN > frame_len){
        return;
    }
    status = get_le32(frame + doffset + 12u);
    if (status != 0u){
        uart_puts("CYW43: control status=");
        uart_putdec(status);
        uart_puts("\n");
    }
}

static int cyw43_handle_escan_frame(const unsigned char* frame,
                                    unsigned int frame_len,
                                    cyw43_scan_result_t* out,
                                    unsigned int cap,
                                    unsigned int* count,
                                    unsigned int* done){
    unsigned int channel = 0;
    unsigned int doffset = 0;
    unsigned int payload_off = 0;
    unsigned int event_type = 0;
    unsigned int status = 0;
    unsigned int data_len = 0;
    unsigned int packet_len = 0;
    unsigned int event_msg_off = 0;
    const unsigned char* event_packet = 0;
    const unsigned char* event_msg = 0;

    if (done){
        *done = 0;
    }
    if (!frame || !out || !count || !done || frame_len < CYW43_SDPCM_HDR_LEN){
        return 0;
    }

    channel = frame[5] & 0x0Fu;
    doffset = frame[7];
    if (doffset < CYW43_SDPCM_HDR_LEN || doffset + CYW43_BDC_HEADER_LEN > frame_len){
        return 0;
    }

    if (channel == CYW43_SDPCM_CH_EVENT){
        /*
         * Some firmware uses the dedicated EVENT channel but still prefixes
         * payload with the normal BDC header.
         */
        payload_off = doffset + CYW43_BDC_HEADER_LEN +
                      ((unsigned int)frame[doffset + 3u] << 2);
    } else if (channel == CYW43_SDPCM_CH_DATA){
        unsigned int bdc_ver = (unsigned int)((frame[doffset] >> CYW43_BDC_VER_SHIFT) & 0x0Fu);
        if (bdc_ver != CYW43_BDC_PROTO_VER){
            return 0;
        }
        /*
         * BCM43436 firmware commonly delivers async events as Ethernet-like
         * packets on the DATA channel. Scan inside the Ethernet payload too.
         */
        payload_off = doffset + CYW43_BDC_HEADER_LEN +
                      ((unsigned int)frame[doffset + 3u] << 2);
    } else{
        return 0;
    }

    if (payload_off >= frame_len){
        return 0;
    }

    event_packet = frame + payload_off;
    packet_len = frame_len - payload_off;
    if (cyw43_find_escan_event_msg(event_packet, packet_len, &event_msg_off) != 0){
        return 0;
    }

    event_msg = event_packet + event_msg_off;
    event_type = get_be32(event_msg + 4u);
    status = get_be32(event_msg + 8u);
    data_len = get_be32(event_msg + 20u);

    if (event_type != CYW43_EV_ESCAN_RESULT){
        return 0;
    }

    if (status == CYW43_STATUS_PARTIAL){
        const unsigned char* ev_scan = event_msg + 48u;
        unsigned int ev_scan_off = payload_off + event_msg_off + 48u;
        unsigned int remain = frame_len - ev_scan_off;
        if (data_len != 0u && data_len < remain){
            remain = data_len;
        }
        if (cyw43_add_scan_event_result(ev_scan, remain, out, cap, count) != 0){
            uart_puts("CYW43: escan partial parse miss len=");
            uart_putdec(remain);
            uart_puts(" data=");
            uart_putdec(data_len);
            uart_puts(" ch=");
            uart_putdec(channel);
            uart_puts("\n");
        }
        return 1;
    }

    if (status == CYW43_STATUS_SUCCESS){
        if (*count == 0u){
            uart_puts("CYW43: escan success zero results data=");
            uart_putdec(data_len);
            uart_puts(" ch=");
            uart_putdec(channel);
            uart_puts("\n");
        }
        *done = 1;
        return 1;
    }

    uart_puts("CYW43: escan status=");
    uart_putdec(status);
    uart_puts("\n");
    *done = 1;
    return 1;
}

int cyw43_get_firmware_version(char* out, unsigned int out_cap){
    unsigned int actual = 0;
    unsigned int n = 0;

    if (!out || out_cap == 0u){
        return -1;
    }
    for (unsigned int i = 0; i < out_cap; i++){
        out[i] = 0;
    }
    if (cyw43_wl_get_var("ver", (unsigned char*)out, out_cap - 1u, &actual) != 0){
        return -1;
    }

    while (n + 1u < out_cap && out[n]){
        if (out[n] == '\r' || out[n] == '\n'){
            out[n] = 0;
            break;
        }
        n++;
    }
    if (n + 1u >= out_cap){
        out[out_cap - 1u] = 0;
    }
    (void)actual;
    return 0;
}

static int cyw43_ioctl_up_common(unsigned int monitor_minimal){
    if (!g_cyw43.enabled){
        if (cyw43_init() != 0){
            return -2;
        }
    }
    if (!g_cyw43.fw_loaded){
        uart_puts("CYW43: UP rejected (firmware not loaded)\n");
        return -1;
    }
    if (g_cyw43.fw_running){
        if (!g_cyw43.func2_ready && cyw43_attach_running_firmware() != 0){
            uart_puts("CYW43: firmware reattach failed\n");
            return -3;
        }
    } else{
        if (cyw43_start_firmware() != 0){
            uart_puts("CYW43: firmware start failed\n");
            return -4;
        }
    }
    if (!monitor_minimal && cyw43_upload_clm_blob() != 0){
        uart_puts("CYW43: CLM load warning; continuing\n");
    }
    if (monitor_minimal){
        /*
         * Nexmon monitor/raw capture does not need CLM or the station-mode
         * setup sequence below. Keep this path intentionally small because
         * patched monitor firmware may apply wl commands without returning
         * normal SDPCM control replies.
         */
        uart_puts("CYW43: monitor minimal up; skipping CLM/station config\n");
        if (cyw43_wl_noresp(CYW43_WLC_UP) != 0){
            uart_puts("CYW43: monitor WLC_UP send failed\n");
            return -6;
        }
        cyw43_delay(200000u);
    } else if (!g_cyw43.wifi_configured){
        if (cyw43_wifi_configure_on() != 0){
            uart_puts("CYW43: WiFi configure-on failed\n");
            return -5;
        }
        g_cyw43.wifi_configured = 1;
    }
    if (!monitor_minimal){
        /*
         * Keep the station path matching the last known-good hidden-join
         * baseline: force RF out of software-disable before WLC_UP instead of
         * relying on a GET_RADIO response first. The control path may be noisy
         * during early bring-up, but SET_RADIO is safe to attempt.
         */
        if (cyw43_wl_set_int(CYW43_WLC_SET_RADIO, 0u) != 0){
            uart_puts("CYW43: radio enable command failed; continuing\n");
        }
        if (cyw43_refresh_cur_etheraddr() != 0){
            uart_puts("CYW43: cur_etheraddr read failed; using nvram MAC\n");
        }
    }
    if (monitor_minimal){
        g_cyw43.iface_up = 1;
        return 0;
    }
    if (!g_cyw43.iface_up &&
        cyw43_wl_cmd(1, CYW43_WLC_UP, 0, 0, 0, 0, 0) != 0){
        /*
         * Some CYW43 firmwares do not return a normal control response while
         * WLC_UP is transitioning the radio. Keep going; the next management
         * command will give us the real up/not-up status.
         */
        uart_puts("CYW43: WLC_UP no reply; continuing\n");
    }
    g_cyw43.iface_up = 1;
    cyw43_log_radio_status("after-up");
    // Latency-oriented defaults for bring-up: keep radio awake and disable
    // minimum power consumption mode while we prioritize responsiveness.
    if (cyw43_wl_set_int(CYW43_WLC_SET_PM, 0u) != 0 ||
        cyw43_wl_set_var_u32("mpc", 0u) != 0){
        uart_puts("CYW43: power-save tuning partial; continuing\n");
    }
    cyw43_drain_pending_packets(16u);
    return 0;
}

int cyw43_ioctl_up(void){
    return cyw43_ioctl_up_common(0u);
}

int cyw43_ioctl_up_monitor(void){
    return cyw43_ioctl_up_common(1u);
}

int cyw43_ioctl_down(void){
    if (g_cyw43.fw_running && g_cyw43.iface_up){
        (void)cyw43_ioctl_monitor(0u, 0u);
        (void)cyw43_wl_cmd(1, CYW43_WLC_DOWN, 0, 0, 0, 0, 0);
    }
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.joined_ssid[0] = 0;
    (void)net_try_select_default_backend();
    return 0;
}

static int cyw43_ioctl_scan_common(const char* ssid,
                                   cyw43_scan_result_t* out,
                                   unsigned int cap,
                                   unsigned int* out_count){
    static unsigned char rx[CYW43_PACKET_MAX_BYTES];
    unsigned int count = 0;
    unsigned int done = 0;
    unsigned int ctrl_frames = 0;
    unsigned int event_frames = 0;
    unsigned int data_frames = 0;
    unsigned int other_frames = 0;

    if (!out_count){
        return -1;
    }
    *out_count = 0;
    if (!g_cyw43.iface_up){
        return -1;
    }
    cyw43_log_radio_status("pre-scan");

    for (unsigned int attempt = 0; attempt < 3u; attempt++){
        count = 0;
        done = 0;
        ctrl_frames = 0;
        event_frames = 0;
        data_frames = 0;
        other_frames = 0;

        if (attempt != 0u){
            cyw43_drain_pending_packets(16u);
            (void)cyw43_wl_set_event_msgs();
            (void)cyw43_wl_set_int(CYW43_WLC_SET_PASSIVE_SCAN, 0u);
            cyw43_delay(80000u);
        }

        if (cyw43_wl_escan_submit(ssid, attempt) != 0){
            if (attempt == 0u){
                uart_puts("CYW43: escan submit retry\n");
                continue;
            }
            uart_puts("CYW43: escan submit failed\n");
            return -1;
        }

        for (unsigned int wait = 0; wait < 32u && !done; wait++){
            unsigned int rx_len = 0;
            unsigned int channel = 0;
            if (cyw43_packet_read(rx, sizeof(rx), &rx_len, 900u, 0) != 0){
                continue;
            }
            if (rx_len >= CYW43_SDPCM_HDR_LEN){
                channel = rx[5] & 0x0Fu;
                if (channel == CYW43_SDPCM_CH_CONTROL){
                    ctrl_frames++;
                } else if (channel == CYW43_SDPCM_CH_EVENT){
                    event_frames++;
                } else if (channel == CYW43_SDPCM_CH_DATA){
                    data_frames++;
                } else{
                    other_frames++;
                }
            }
            cyw43_log_control_status(rx, rx_len);
            (void)cyw43_handle_escan_frame(rx, rx_len, out, cap, &count, &done);
        }

        *out_count = count;
        g_cyw43.last_scan_count = count;
        if (count > 0u){
            return 0;
        }
        if (done){
            uart_puts("CYW43: escan completed with zero APs; trying fallback\n");
            if (cyw43_collect_scanresults(out, cap, &count) == 0){
                *out_count = count;
                g_cyw43.last_scan_count = count;
                return 0;
            }
        }

        if (attempt == 0u){
            uart_puts("CYW43: escan retry after timeout\n");
        }
    }

    uart_puts("CYW43: trying legacy WLC_SCAN fallback\n");
    count = 0;
    if (cyw43_wl_legacy_scan_submit(ssid) == 0){
        for (unsigned int wait = 0; wait < 8u; wait++){
            unsigned int rx_len = 0;
            if (cyw43_packet_read(rx, sizeof(rx), &rx_len, 500u, 1) != 0){
                continue;
            }
            if (rx_len == 0u){
                continue;
            }
            cyw43_log_control_status(rx, rx_len);
        }
        if (cyw43_collect_scanresults(out, cap, &count) == 0){
            *out_count = count;
            g_cyw43.last_scan_count = count;
            return 0;
        }
    } else{
        uart_puts("CYW43: legacy WLC_SCAN submit failed\n");
    }

    uart_puts("CYW43: escan timed out ctrl=");
    uart_putdec(ctrl_frames);
    uart_puts(" event=");
    uart_putdec(event_frames);
    uart_puts(" data=");
    uart_putdec(data_frames);
    uart_puts(" other=");
    uart_putdec(other_frames);
    uart_puts("\n");
    return -1;
}

int cyw43_ioctl_scan(cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count){
    return cyw43_ioctl_scan_common(0, out, cap, out_count);
}

int cyw43_ioctl_scan_ssid(const char* ssid, cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count){
    if (!ssid || !*ssid){
        return cyw43_ioctl_scan_common(0, out, cap, out_count);
    }
    return cyw43_ioctl_scan_common(ssid, out, cap, out_count);
}

int cyw43_ioctl_join(const char* ssid, const char* password){
    unsigned int n = 0;
    unsigned int wpa_auth = CYW43_WPA_AUTH_DISABLED;
    unsigned char gateway_ip[4];

    if (!ssid || !*ssid || !g_cyw43.iface_up){
        return -1;
    }
    n = strn_len_local(ssid, 32u);
    if (n == 0u || n > CYW43_WL_MAX_SSID_LEN){
        return -1;
    }

    if (password && *password){
        wpa_auth = CYW43_WPA2_AUTH_PSK;
        if (cyw43_wl_set_int(CYW43_WLC_SET_WSEC, CYW43_WSEC_AES) != 0 ||
            cyw43_wl_prepare_sta_supplicant() != 0){
            uart_puts("CYW43: join WPA setup failed\n");
            return -1;
        }
        if (cyw43_wl_set_int(CYW43_WLC_SET_WPA_AUTH, wpa_auth) != 0){
            uart_puts("CYW43: join WPA auth mode setup failed\n");
            return -1;
        }
        /*
         * CYW43 firmware can reject PMK writes if we push too soon after
         * supplicant iovar setup. Keep a short settle delay.
         */
        cyw43_delay(200000u);
        if (cyw43_wl_set_pmk(password) != 0){
            /*
             * Compatibility fallback for APs/firmware that insist on mixed
             * WPA/WPA2 auth mode while using AES.
             */
            wpa_auth = CYW43_WPA_AUTH_PSK | CYW43_WPA2_AUTH_PSK;
            if (cyw43_wl_set_int(CYW43_WLC_SET_WPA_AUTH, wpa_auth) != 0){
                uart_puts("CYW43: join WPA auth fallback setup failed\n");
                return -1;
            }
            cyw43_delay(200000u);
            if (cyw43_wl_set_pmk(password) != 0){
                uart_puts("CYW43: join WPA setup failed\n");
                return -1;
            }
        }
    } else{
        if (cyw43_wl_set_int(CYW43_WLC_SET_WSEC, 0u) != 0 ||
            cyw43_wl_set_int(CYW43_WLC_SET_WPA_AUTH, CYW43_WPA_AUTH_DISABLED) != 0){
            uart_puts("CYW43: join open setup failed\n");
            return -1;
        }
    }

    if (cyw43_wl_set_int(CYW43_WLC_SET_INFRA, 1u) != 0 ||
        cyw43_wl_set_int(CYW43_WLC_SET_AUTH, 0u) != 0 ||
        cyw43_wl_set_int(CYW43_WLC_SET_WPA_AUTH, wpa_auth) != 0){
        uart_puts("CYW43: join basic mode setup failed\n");
        return -1;
    }

    if (cyw43_wl_set_ssid_cmd(CYW43_WLC_SET_SSID, ssid) != 0){
        uart_puts("CYW43: set ssid failed\n");
        return -1;
    }

    for (unsigned int i = 0; i < n; i++){
        g_cyw43.joined_ssid[i] = ssid[i];
    }
    g_cyw43.joined_ssid[n] = 0;
    if (cyw43_wait_assoc(2500u) != 0){
        // Keep join non-fatal here; some firmware builds associate slightly
        // later even after SET_SSID returned success.
        uart_puts("CYW43: association pending\n");
    }
    cyw43_drain_pending_packets(64u);

    (void)cyw43_refresh_cur_etheraddr();
    g_cyw43.joined = 1;
    net_proto_set_local_mac(g_cyw43.mac);
    if (net_try_select_wifi_backend() != 0){
        g_cyw43.joined = 0;
        g_cyw43.joined_ssid[0] = 0;
        uart_puts("CYW43: join backend switch failed\n");
        return -1;
    }

    /*
     * Re-arm gateway ARP resolution when switching from Ethernet/stub to Wi-Fi,
     * then attempt an early resolve so first ping has a cached gateway MAC.
     */
    net_proto_get_gateway_ip(gateway_ip);
    arp_set_periodic_target(gateway_ip, 1000u);
    (void)arp_send_request(gateway_ip);
    return 0;
}

static int cyw43_net_deliver_data(const unsigned char* packet, unsigned int packet_len){
    unsigned int doffset = 0;
    const unsigned char* bdc = 0;
    unsigned int bdc_ver = 0;
    unsigned int eth_off = 0;
    unsigned int eth_len = 0;

    if (!packet || packet_len < CYW43_SDPCM_HDR_LEN + CYW43_BDC_HEADER_LEN){
        return -1;
    }

    doffset = packet[7];
    if (doffset < CYW43_SDPCM_HDR_LEN || doffset + CYW43_BDC_HEADER_LEN > packet_len){
        return -1;
    }

    bdc = packet + doffset;
    bdc_ver = (unsigned int)((bdc[0] >> CYW43_BDC_VER_SHIFT) & 0x0Fu);
    if (bdc_ver != CYW43_BDC_PROTO_VER){
        /*
         * Nexmon monitor firmware can deliver radiotap/802.11 payloads that do
         * not use the normal Broadcom BDC Ethernet header. Keep those visible
         * to the raw capture queue, but do not pass them to the Ethernet stack.
         */
        cyw43_raw_capture_push(packet + doffset, packet_len - doffset);
        return -1;
    }

    eth_off = doffset + CYW43_BDC_HEADER_LEN + ((unsigned int)bdc[3] << 2);
    if (eth_off >= packet_len){
        return -1;
    }
    eth_len = packet_len - eth_off;
    /*
     * This is intentionally below the Ethernet stack. With stock firmware it
     * captures Ethernet payloads; with a Nexmon-style monitor firmware it can
     * capture raw 802.11/radiotap-like payloads without needing to rewrite the
     * normal IP path.
     */
    cyw43_raw_capture_push(packet + eth_off, eth_len);
    if (eth_len < 14u || eth_len > NET_MAX_FRAME_SIZE){
        return -1;
    }

    if (g_cyw43_rx_handler){
        g_cyw43_rx_handler(packet + eth_off, eth_len);
    }
    return 1;
}

int cyw43_net_set_rx_handler(cyw43_rx_handler_t handler){
    g_cyw43_rx_handler = handler;
    return 0;
}

int cyw43_net_ready(void){
    return (g_cyw43.fw_running && g_cyw43.iface_up && g_cyw43.joined && g_cyw43.func2_ready) ? 1 : 0;
}

int cyw43_net_link_up(void){
    return cyw43_net_ready();
}

int cyw43_net_send_ethernet(const unsigned char* frame, unsigned int len){
    static unsigned char tx[CYW43_PACKET_MAX_BYTES];
    unsigned int payload_len = 0;
    unsigned int raw_len = 0;
    unsigned int frame_len = 0;

    if (!cyw43_net_ready() || !frame || len < 14u || len > NET_MAX_FRAME_SIZE){
        return -1;
    }
    if (cyw43_sdio_keep_awake() != 0){
        return -1;
    }

    payload_len = CYW43_DATA_PAD_LEN + CYW43_BDC_HEADER_LEN + len;
    raw_len = CYW43_SDPCM_HDR_LEN + payload_len;
    frame_len = round4_u32(raw_len);
    if (frame_len > sizeof(tx)){
        return -1;
    }

    mem_zero_local(tx, sizeof(tx));
    put_le16(tx + 0u, raw_len);
    put_le16(tx + 2u, raw_len ^ 0xFFFFu);
    tx[4] = (unsigned char)(g_cyw43.sdpcm_tx_seq & 0xFFu);
    tx[5] = CYW43_SDPCM_CH_DATA;
    tx[6] = 0u;
    tx[7] = CYW43_SDPCM_HDR_LEN + CYW43_DATA_PAD_LEN;
    tx[8] = 0u;
    tx[9] = 0u;
    tx[10] = 0u;
    tx[11] = 0u;

    // Data channel uses two bytes of padding before the BDC header.
    unsigned int bdc_off = CYW43_SDPCM_HDR_LEN + CYW43_DATA_PAD_LEN;
    tx[bdc_off + 0u] = (unsigned char)(CYW43_BDC_PROTO_VER << CYW43_BDC_VER_SHIFT);
    tx[bdc_off + 1u] = 0u;
    tx[bdc_off + 2u] = 0u; // interface 0 (STA)
    tx[bdc_off + 3u] = 0u;

    mem_copy_local(tx + bdc_off + CYW43_BDC_HEADER_LEN, frame, len);

    if (cyw43_packet_write(tx, frame_len) != 0){
        g_cyw43.net_tx_fail++;
        return -1;
    }
    g_cyw43.sdpcm_tx_seq++;
    g_cyw43.net_tx_ok++;
    return 0;
}

int cyw43_net_poll(void){
    static unsigned char rx[CYW43_PACKET_MAX_BYTES];
    int delivered = 0;

    if (!cyw43_net_ready()){
        return 0;
    }

    for (unsigned int i = 0; i < 16u; i++){
        unsigned int rx_len = 0;
        unsigned int channel = 0;

        if (cyw43_packet_read(rx, sizeof(rx), &rx_len, 0u, 1) != 0){
            break;
        }
        if (rx_len < CYW43_SDPCM_HDR_LEN){
            continue;
        }
        channel = rx[5] & 0x0Fu;
        if (channel == CYW43_SDPCM_CH_DATA){
            g_cyw43.net_rx_data++;
            if (cyw43_net_deliver_data(rx, rx_len) > 0){
                delivered++;
            }
        } else if (channel == CYW43_SDPCM_CH_EVENT){
            g_cyw43.net_rx_event++;
        } else if (channel == CYW43_SDPCM_CH_CONTROL){
            g_cyw43.net_rx_control++;
        } else{
            g_cyw43.net_rx_other++;
        }
    }
    return delivered;
}

static int cyw43_raw_capture_poll_budget(unsigned int max_frames,
                                         unsigned int first_wait_ms){
    static unsigned char rx[CYW43_PACKET_MAX_BYTES];
    int delivered = 0;

    if (!spin_trylock(&g_cyw43_raw_poll_lock)){
        return 0;
    }

    if (!g_cyw43_raw_enabled ||
        !g_cyw43.fw_running ||
        !g_cyw43.func2_ready){
        spin_unlock(&g_cyw43_raw_poll_lock);
        return 0;
    }

    for (unsigned int i = 0; i < max_frames; i++){
        unsigned int rx_len = 0;
        unsigned int channel = 0;

        /*
         * Never let monitor capture block foreground typing/rendering. The
         * caller decides whether a short wait is acceptable; background polls
         * use zero wait and only skim a couple of queued frames.
         */
        if (cyw43_packet_read(rx, sizeof(rx), &rx_len, (i == 0u) ? first_wait_ms : 0u, 1) != 0){
            break;
        }
        if (rx_len < CYW43_SDPCM_HDR_LEN){
            continue;
        }
        channel = rx[5] & 0x0Fu;
        if (channel == CYW43_SDPCM_CH_DATA){
            g_cyw43.net_rx_data++;
            if (cyw43_net_deliver_data(rx, rx_len) > 0){
                delivered++;
            }
        } else if (channel == CYW43_SDPCM_CH_EVENT){
            g_cyw43.net_rx_event++;
        } else if (channel == CYW43_SDPCM_CH_CONTROL){
            g_cyw43.net_rx_control++;
        } else{
            g_cyw43.net_rx_other++;
        }
    }
    spin_unlock(&g_cyw43_raw_poll_lock);
    return delivered;
}

int cyw43_raw_capture_poll(void){
    return cyw43_raw_capture_poll_budget(16u, 3u);
}

int cyw43_raw_capture_poll_lite(void){
    return cyw43_raw_capture_poll_budget(2u, 0u);
}

int cyw43_get_status(cyw43_status_t* out){
    if (!out){
        return -1;
    }
    out->enabled = g_cyw43.enabled;
    out->ready = sdio_bus_is_ready() ? 1u : 0u;
    out->func1_ready = g_cyw43.func1_ready;
    out->func2_ready = g_cyw43.func2_ready;
    out->fw_loaded = g_cyw43.fw_loaded;
    out->fw_running = g_cyw43.fw_running;
    out->iface_up = g_cyw43.iface_up;
    out->joined = g_cyw43.joined;
    out->mac[0] = g_cyw43.mac[0];
    out->mac[1] = g_cyw43.mac[1];
    out->mac[2] = g_cyw43.mac[2];
    out->mac[3] = g_cyw43.mac[3];
    out->mac[4] = g_cyw43.mac[4];
    out->mac[5] = g_cyw43.mac[5];
    out->country[0] = g_cyw43.country[0];
    out->country[1] = g_cyw43.country[1];
    out->country[2] = g_cyw43.country[2];
    out->country[3] = 0;
    out->sdpcm_tx_seq = g_cyw43.sdpcm_tx_seq;
    out->last_scan_count = g_cyw43.last_scan_count;
    return 0;
}

void cyw43_dump_status(void){
    uart_puts("CYW43 enabled=");
    uart_putdec(g_cyw43.enabled);
    uart_puts(" sdio_ready=");
    uart_putdec(sdio_bus_is_ready() ? 1u : 0u);
    uart_puts(" f1=");
    uart_putdec(g_cyw43.func1_ready);
    uart_puts(" f2=");
    uart_putdec(g_cyw43.func2_ready);
    uart_puts(" fw=");
    uart_putdec(g_cyw43.fw_loaded);
    uart_puts(" run=");
    uart_putdec(g_cyw43.fw_running);
    uart_puts(" up=");
    uart_putdec(g_cyw43.iface_up);
    uart_puts(" joined=");
    uart_putdec(g_cyw43.joined);
    uart_puts(" scan=");
    uart_putdec(g_cyw43.last_scan_count);
    uart_puts("\n");
    uart_puts("CYW43 MAC=");
    for (unsigned int i = 0; i < 6u; i++){
        uart_puthex(g_cyw43.mac[i]);
        if (i + 1u < 6u){
            uart_puts(":");
        }
    }
    uart_puts(" country=");
    uart_puts(g_cyw43.country[0] ? g_cyw43.country : "??");
    uart_puts(" rev=");
    uart_putdec(g_cyw43.country_rev);
    uart_puts("\n");
    if (g_cyw43.fw_running && g_cyw43.func2_ready){
        unsigned char rfc0 = 0;
        unsigned char rfc1 = 0;
        unsigned char intpend = 0;
        unsigned int ints = 0;
        unsigned int mbox = 0;
        cyw43_log_radio_status("dump");
        (void)sdio_bus_cmd52_read(1, CYW43_RFRAME_COUNT_REG, &rfc0);
        (void)sdio_bus_cmd52_read(1, CYW43_RFRAME_COUNT_REG + 1u, &rfc1);
        (void)sdio_bus_cmd52_read(0, 0x05u, &intpend);
        if (g_cyw43.sd_regs != 0u){
            (void)cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, &ints);
            (void)cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_HOSTMBOX_DATA, &mbox);
        }
        uart_puts("CYW43 sdio rfcnt=");
        uart_puthex(((unsigned int)rfc1 << 8) | rfc0);
        uart_puts(" pend=");
        uart_puthex(intpend);
        uart_puts(" ints=");
        uart_puthex(ints);
        uart_puts(" host=");
        uart_puthex(ints & CYW43_SD_HOST_INT_MASK);
        uart_puts(" mbox=");
        uart_puthex(mbox);
        uart_puts(" seq=");
        uart_putdec(g_cyw43.sdpcm_tx_seq & 0xFFu);
        uart_puts(" win=");
        uart_putdec(g_cyw43.tx_window);
        uart_puts(" flow=");
        uart_puthex(g_cyw43.flow_mask);
        uart_puts("\n");
    }
    uart_puts("CYW43 net tx_ok=");
    uart_putdec(g_cyw43.net_tx_ok);
    uart_puts(" tx_fail=");
    uart_putdec(g_cyw43.net_tx_fail);
    uart_puts(" rx_data=");
    uart_putdec(g_cyw43.net_rx_data);
    uart_puts(" rx_event=");
    uart_putdec(g_cyw43.net_rx_event);
    uart_puts(" rx_ctl=");
    uart_putdec(g_cyw43.net_rx_control);
    uart_puts(" rx_other=");
    uart_putdec(g_cyw43.net_rx_other);
    uart_puts("\n");
}
