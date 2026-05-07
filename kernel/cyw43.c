#include "cyw43.h"
#include "sdio_bus.h"
#include "fat32.h"
#include "blockdev.h"
#include "memory.h"
#include "uart.h"

#define CYW43_FW_MAX_BYTES      (768u * 1024u)
#define CYW43_NVRAM_MAX_BYTES   (16u * 1024u)
#define CYW43_NVRAM_PACKED_MAX  (20u * 1024u)
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
#define CYW43_SD_INT_CHIPACTIVE (1u << 29)
#define CYW43_SD_HOST_INT_MASK  (CYW43_SD_INT_HOST_MASK | CYW43_SD_INT_CHIPACTIVE)
#define CYW43_SD_FW_READY       0x80u
#define CYW43_SD_FW_READY_ALT   0x08u // Circle's intwait path checks this bit.

#define CYW43_CRESCAN_SIZE      512u
#define CYW43_SDPCM_HDR_LEN     12u
#define CYW43_SDPCM_FIRSTREAD   64u
#define CYW43_CDC_HDR_LEN       16u
#define CYW43_PACKET_MAX_BYTES  2048u
#define CYW43_PACKET_ADDR       CYW43_SB_32BIT_ADDR
#define CYW43_WLC_UP            2u
#define CYW43_WLC_DOWN          3u
#define CYW43_WLC_SET_INFRA     20u
#define CYW43_WLC_SET_AUTH      22u
#define CYW43_WLC_SET_SSID      26u
#define CYW43_WLC_SCAN          50u
#define CYW43_WLC_SCAN_RESULTS  51u
#define CYW43_WLC_SET_WSEC      134u
#define CYW43_WLC_SET_WPA_AUTH  165u
#define CYW43_WLC_GET_VAR       262u
#define CYW43_WLC_SET_VAR       263u
#define CYW43_WLC_SET_WSEC_PMK  268u

#define CYW43_WL_BSS_INFO_VER   109u
#define CYW43_WL_SCAN_BUF_LEN   1800u
#define CYW43_WL_SCAN_PARAMS_LEN 64u
#define CYW43_WL_MAX_SSID_LEN   32u
#define CYW43_DOT11_BSSTYPE_ANY 2u
#define CYW43_WSEC_AES          0x0004u
#define CYW43_WSEC_PASSPHRASE   0x0001u
#define CYW43_WPA_AUTH_DISABLED 0x0000u
#define CYW43_WPA_AUTH_PSK      0x0004u
#define CYW43_WPA2_AUTH_PSK     0x0080u

// CYW43430/CYW43438 firmware RAM layout. The full Circle path discovers this
// by scanning cores; this bootstrap path uses the known Pi 3/Zero 2W value.
#define CYW43_RAM_BASE          0x00000000u
#define CYW43_RAM_SIZE          0x000C8000u

typedef struct {
    unsigned char enabled;
    unsigned char func1_ready;
    unsigned char func2_ready;
    unsigned char fw_loaded;
    unsigned char fw_running;
    unsigned char iface_up;
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
    char country[3];
    unsigned int sdpcm_tx_seq;
    unsigned short reqid;
    unsigned char flow_mask;
    unsigned char tx_window;
    unsigned int last_scan_count;
    char joined_ssid[33];
} cyw43_state_t;

static cyw43_state_t g_cyw43;

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
            if (vlen >= 2u){
                g_cyw43.country[0] = text[eq + 1u];
                g_cyw43.country[1] = text[eq + 2u];
                g_cyw43.country[2] = 0;
                have_country = 1;
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
    }

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

static int cyw43_enable_ht_clock(void){
    unsigned char csr = 0;

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
    uart_puts("CYW43: waiting firmware mailbox\n");
    for (unsigned int i = 0; i < 100u; i++){
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
            if (mbox & (CYW43_SD_FW_READY | CYW43_SD_FW_READY_ALT)){
                (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX, 2u);
                uart_puts("CYW43: firmware ready\n");
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
    if (cyw43_enable_ht_clock() != 0){
        return -1;
    }
    uart_puts("CYW43: programming SDIO mailbox/intmask\n");
    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_STATUS, 0xFFFFFFFFu);
    if (cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX_DATA, 4u << 16) != 0){
        uart_puts("CYW43: SDIO interrupt setup failed\n");
        return -1;
    }
    uart_puts("CYW43: SDIO mailbox OK\n");
    if (cyw43_enable_function2() != 0){
        return -1;
    }
    cyw43_program_f2_watermark();
    if (cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_INT_MASK,
                                CYW43_SD_HOST_INT_MASK) != 0){
        uart_puts("CYW43: SDIO interrupt mask failed\n");
        return -1;
    }
    uart_puts("CYW43: SDIO intmask OK\n");
    if (cyw43_wait_firmware_ready() != 0){
        return -1;
    }
    if (cyw43_sdio_keep_awake() != 0){
        uart_puts("CYW43: SDIO wake warning; continuing\n");
    }
    g_cyw43.fw_running = 1;
    return 0;
}

static void cyw43_drop_sdio_state(void){
    g_cyw43.enabled = 0;
    g_cyw43.func1_ready = 0;
    g_cyw43.func2_ready = 0;
    g_cyw43.fw_running = 0;
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.reqid = 0;
    g_cyw43.flow_mask = 0;
    g_cyw43.tx_window = 0;
    sdio_bus_reset_state();
    blockdev_reserve_emmc_for_wifi(0);
}

int cyw43_init(void){
    if (g_cyw43.enabled){
        return 0;
    }

    if (sdio_bus_init() != 0){
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
    g_cyw43.fw_running = 0;
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.sdpcm_tx_seq = 0;
    g_cyw43.reqid = 0;
    g_cyw43.flow_mask = 0;
    g_cyw43.tx_window = 0;
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
    uart_puts("CYW43: firmware + NVRAM staged over SDIO\n");
    return 0;
}

int cyw43_upload_firmware_from_fat(const char* fw_bin_83, const char* nvram_txt_83){
    const char* fw_name = fw_bin_83 ? fw_bin_83 : "4343WIFIBIN";
    const char* nv_name = nvram_txt_83 ? nvram_txt_83 : "4343NVRMTXT";
    unsigned char* fw_buf = 0;
    unsigned char* nv_buf = 0;
    int fw_len = -1;
    int nv_len = -1;
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

static int cyw43_wait_rx_frame(unsigned int timeout_ms){
    unsigned char count0 = 0;
    unsigned char count1 = 0;
    unsigned char intpend = 0;
    unsigned int ints = 0;
    unsigned int mbox = 0;
    unsigned int loops = timeout_ms * 20u;

    if (loops < 1000u){
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
                unsigned int ack = ints & ~CYW43_SD_INT_FRAME;
                if (ints & CYW43_SD_INT_MAILBOX){
                    (void)cyw43_backplane_read32(g_cyw43.sd_regs + CYW43_SD_HOSTMBOX_DATA, &mbox);
                    (void)cyw43_backplane_write32(g_cyw43.sd_regs + CYW43_SD_SBMBOX, 2u);
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

    uart_puts("CYW43: rx wait timeout rfcnt=");
    uart_puthex(((unsigned int)count1 << 8) | count0);
    uart_puts(" pend=");
    uart_puthex(intpend);
    uart_puts(" ints=");
    uart_puthex(ints);
    uart_puts(" mbox=");
    uart_puthex(mbox);
    uart_puts("\n");
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
     * Broadcom SDIO Function 2 packet I/O is addressed through the selected
     * backplane window. Linux brcmfmac points the window at chipcommon
     * (0x18000000) and uses address 0x8000 for 4-byte packet access.
     */
    return cyw43_backplane_window(CYW43_ENUM_BASE);
}

static int cyw43_packet_write(const unsigned char* data, unsigned int len){
    unsigned int xfer_len = round4_u32(len);
    if (!data || len < CYW43_SDPCM_HDR_LEN || xfer_len > CYW43_PACKET_MAX_BYTES){
        return -1;
    }
    if (cyw43_prepare_packet_window() != 0){
        return -1;
    }
    return sdio_bus_cmd53_write(2, CYW43_PACKET_ADDR, data, xfer_len);
}

static int cyw43_packet_read(unsigned char* out, unsigned int out_cap, unsigned int* out_len){
    unsigned int len = 0;
    unsigned int lenck = 0;
    unsigned int first_read = CYW43_SDPCM_FIRSTREAD;
    unsigned int remain_len = 0;
    unsigned int remain_xfer = 0;

    if (!out || !out_len || out_cap < CYW43_SDPCM_HDR_LEN){
        return -1;
    }
    *out_len = 0;
    if (first_read > out_cap){
        first_read = 4u;
    }

    if (cyw43_wait_rx_frame(1500u) != 0){
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
        uart_puts("CYW43: bad SDPCM len=");
        uart_puthex(len);
        uart_puts(" lenck=");
        uart_puthex(lenck);
        uart_puts("\n");
        cyw43_rx_halt_and_drain();
        return -1;
    }

    if (len > first_read){
        remain_len = len - first_read;
        remain_xfer = round4_u32(remain_len);
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
    put_le16(tx + 0u, frame_len);
    put_le16(tx + 2u, frame_len ^ 0xFFFFu);
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

    for (unsigned int tries = 0; tries < 3u; tries++){
        unsigned int rx_len = 0;
        unsigned int channel = 0;
        unsigned int doffset = 0;
        unsigned int status = 0;
        unsigned int cdc_len = 0;
        unsigned int copy_len = 0;

        if (cyw43_packet_read(rx, sizeof(rx), &rx_len) != 0){
            continue;
        }
        if (rx_len == 0u){
            continue;
        }

        g_cyw43.flow_mask = rx[8];
        g_cyw43.tx_window = rx[9];
        channel = rx[5] & 0x0Fu;
        doffset = rx[7];
        if (channel != CYW43_SDPCM_CH_CONTROL){
            continue;
        }
        if (doffset < CYW43_SDPCM_HDR_LEN ||
            doffset + CYW43_CDC_HDR_LEN > rx_len){
            continue;
        }
        if ((unsigned short)get_le16(rx + doffset + 10u) != reqid){
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

static int cyw43_wl_set_int(unsigned int op, unsigned int value){
    unsigned char buf[4];
    put_le32(buf, value);
    return cyw43_wl_cmd(1, op, buf, sizeof(buf), 0, 0, 0);
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

static int cyw43_wl_scan_submit(void){
    unsigned char params[CYW43_WL_SCAN_PARAMS_LEN];

    mem_zero_local(params, sizeof(params));
    /*
     * wl_scan_params:
     *   wlc_ssid_t ssid;        // len 0 = wildcard
     *   ether_addr bssid;       // ff:ff:ff:ff:ff:ff = broadcast scan
     *   int8 bss_type;          // any
     *   uint8 scan_type;        // default active/passive policy
     *   int32 nprobes/active/passive/home_time; -1 = firmware default
     *   int32 channel_num;      // 0 = all channels, no appended channel list
     */
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
    put_le16(pmk + 2u, (len < 64u) ? CYW43_WSEC_PASSPHRASE : 0u);
    for (unsigned int i = 0; i < len; i++){
        pmk[4u + i] = (unsigned char)password[i];
    }

    return cyw43_wl_cmd(1, CYW43_WLC_SET_WSEC_PMK, pmk, sizeof(pmk), 0, 0, 0);
}

static unsigned int cyw43_bss_channel(const unsigned char* bss, unsigned int ver){
    if (ver == 107u){
        return (unsigned int)bss[72u];
    }

    /*
     * For 4343x chanspecs the low byte is the primary channel. ctl_ch is a
     * useful fallback for modern version-109 BSS records.
     */
    unsigned int ch = get_le16(bss + 72u) & 0xFFu;
    if (ch == 0u){
        ch = (unsigned int)bss[88u];
    }
    return ch;
}

static void cyw43_parse_scan_results(const unsigned char* buf, unsigned int buf_len,
                                     cyw43_scan_result_t* out, unsigned int cap,
                                     unsigned int* out_count){
    unsigned int firmware_count = 0;
    unsigned int pos = 12u;
    unsigned int count = 0;

    if (!out_count){
        return;
    }
    *out_count = 0;
    if (!buf || !out || cap == 0u || buf_len < 12u){
        return;
    }

    firmware_count = get_le32(buf + 8u);
    for (unsigned int i = 0; i < firmware_count && count < cap; i++){
        const unsigned char* bss = buf + pos;
        unsigned int ver = 0;
        unsigned int len = 0;
        unsigned int ssid_len = 0;
        unsigned int capinfo = 0;

        if (pos + 82u > buf_len){
            break;
        }
        ver = get_le32(bss + 0u);
        len = get_le32(bss + 4u);
        if (len < 82u || pos + len > buf_len){
            break;
        }

        ssid_len = (unsigned int)bss[18u];
        if (ssid_len > CYW43_WL_MAX_SSID_LEN){
            ssid_len = CYW43_WL_MAX_SSID_LEN;
        }
        for (unsigned int s = 0; s < ssid_len; s++){
            out[count].ssid[s] = (char)bss[19u + s];
        }
        out[count].ssid[ssid_len] = 0;
        if (ssid_len == 0u){
            out[count].ssid[0] = '<';
            out[count].ssid[1] = 'h';
            out[count].ssid[2] = 'i';
            out[count].ssid[3] = 'd';
            out[count].ssid[4] = 'd';
            out[count].ssid[5] = 'e';
            out[count].ssid[6] = 'n';
            out[count].ssid[7] = '>';
            out[count].ssid[8] = 0;
        }

        capinfo = get_le16(bss + 16u);
        out[count].channel = (unsigned char)cyw43_bss_channel(bss, ver);
        out[count].rssi_dbm = get_le16s(bss + 78u);
        out[count].auth = (capinfo & 0x0010u) ? 3u : 0u;
        count++;

        pos += len;
    }

    *out_count = count;
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

int cyw43_ioctl_up(void){
    if (!g_cyw43.enabled){
        if (cyw43_init() != 0){
            return -1;
        }
    }
    if (!g_cyw43.fw_loaded){
        uart_puts("CYW43: UP rejected (firmware not loaded)\n");
        return -1;
    }
    if (!g_cyw43.fw_running && cyw43_start_firmware() != 0){
        uart_puts("CYW43: firmware start failed\n");
        return -1;
    }
    if (!g_cyw43.iface_up &&
        cyw43_wl_cmd(1, CYW43_WLC_UP, 0, 0, 0, 0, 0) != 0){
        uart_puts("CYW43: WLC_UP failed\n");
        return -1;
    }
    g_cyw43.iface_up = 1;
    return 0;
}

int cyw43_ioctl_down(void){
    if (g_cyw43.fw_running && g_cyw43.iface_up){
        (void)cyw43_wl_cmd(1, CYW43_WLC_DOWN, 0, 0, 0, 0, 0);
    }
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.joined_ssid[0] = 0;
    return 0;
}

int cyw43_ioctl_scan(cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count){
    static unsigned char results_buf[CYW43_WL_SCAN_BUF_LEN];
    unsigned int actual = 0;

    if (!out_count){
        return -1;
    }
    *out_count = 0;
    if (!g_cyw43.iface_up){
        return -1;
    }

    if (cyw43_wl_scan_submit() != 0){
        uart_puts("CYW43: scan submit failed\n");
        return -1;
    }

    /*
     * Minimal blocking scan path. Later we should consume firmware events and
     * make this asynchronous, but the first goal is to prove real radio results.
     */
    for (unsigned int wait = 0; wait < 16u; wait++){
        mem_zero_local(results_buf, sizeof(results_buf));
        put_le32(results_buf + 0u, sizeof(results_buf));
        put_le32(results_buf + 4u, CYW43_WL_BSS_INFO_VER);
        put_le32(results_buf + 8u, 0u);

        if (cyw43_wl_cmd(0, CYW43_WLC_SCAN_RESULTS,
                         results_buf, 12u,
                         results_buf, sizeof(results_buf),
                         &actual) == 0){
            cyw43_parse_scan_results(results_buf, actual, out, cap, out_count);
            if (*out_count > 0u || wait + 1u >= 16u){
                g_cyw43.last_scan_count = *out_count;
                return 0;
            }
        }
        cyw43_delay(500000u);
    }

    uart_puts("CYW43: scan results failed\n");
    return -1;
}

int cyw43_ioctl_join(const char* ssid, const char* password){
    unsigned int n = 0;

    if (!ssid || !*ssid || !g_cyw43.iface_up){
        return -1;
    }
    n = strn_len_local(ssid, 32u);
    if (n == 0u || n > CYW43_WL_MAX_SSID_LEN){
        return -1;
    }

    if (cyw43_wl_set_int(CYW43_WLC_SET_INFRA, 1u) != 0 ||
        cyw43_wl_set_int(CYW43_WLC_SET_AUTH, 0u) != 0){
        uart_puts("CYW43: join basic mode setup failed\n");
        return -1;
    }

    if (password && *password){
        if (cyw43_wl_set_int(CYW43_WLC_SET_WSEC, CYW43_WSEC_AES) != 0 ||
            cyw43_wl_set_int(CYW43_WLC_SET_WPA_AUTH,
                             CYW43_WPA_AUTH_PSK | CYW43_WPA2_AUTH_PSK) != 0 ||
            cyw43_wl_set_pmk(password) != 0){
            uart_puts("CYW43: join WPA setup failed\n");
            return -1;
        }
    } else{
        if (cyw43_wl_set_int(CYW43_WLC_SET_WSEC, 0u) != 0 ||
            cyw43_wl_set_int(CYW43_WLC_SET_WPA_AUTH, CYW43_WPA_AUTH_DISABLED) != 0){
            uart_puts("CYW43: join open setup failed\n");
            return -1;
        }
    }

    if (cyw43_wl_set_ssid_cmd(CYW43_WLC_SET_SSID, ssid) != 0){
        uart_puts("CYW43: set ssid failed\n");
        return -1;
    }

    for (unsigned int i = 0; i < n; i++){
        g_cyw43.joined_ssid[i] = ssid[i];
    }
    g_cyw43.joined_ssid[n] = 0;
    g_cyw43.joined = 1;
    return 0;
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
    out->country[2] = 0;
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
    uart_puts("\n");
}
