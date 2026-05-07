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
#define CYW43_CLK_ALP_AVAIL     0x40u
#define CYW43_ENUM_BASE         0x18000000u

// CYW43430/CYW43438 firmware RAM layout. The full Circle path discovers this
// by scanning cores; this bootstrap path uses the known Pi 3/Zero 2W value.
#define CYW43_RAM_BASE          0x00000000u
#define CYW43_RAM_SIZE          0x000C8000u

typedef struct {
    unsigned char enabled;
    unsigned char func1_ready;
    unsigned char func2_ready;
    unsigned char fw_loaded;
    unsigned char iface_up;
    unsigned char joined;
    unsigned char mac[6];
    char country[3];
    unsigned int sdpcm_tx_seq;
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

static unsigned int get_le32(const unsigned char in[4]){
    return ((unsigned int)in[0]) |
           ((unsigned int)in[1] << 8) |
           ((unsigned int)in[2] << 16) |
           ((unsigned int)in[3] << 24);
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

static int cyw43_request_alp_clock(void){
    unsigned char csr = 0;
    if (sdio_bus_cmd52_write(1, CYW43_CLKCSR_REG, CYW43_CLK_REQ_ALP) != 0){
        return -1;
    }
    for (unsigned int i = 0; i < 200000u; i++){
        if (sdio_bus_cmd52_read(1, CYW43_CLKCSR_REG, &csr) == 0 &&
            (csr & CYW43_CLK_ALP_AVAIL)){
            return 0;
        }
        asm volatile("nop");
    }
    uart_puts("CYW43: ALP clock timeout csr=");
    uart_puthex(csr);
    uart_puts("\n");
    return -1;
}

static void cyw43_drop_sdio_state(void){
    g_cyw43.enabled = 0;
    g_cyw43.func1_ready = 0;
    g_cyw43.func2_ready = 0;
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
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
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.sdpcm_tx_seq = 0;
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
    unsigned int chip_id = 0;
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

    if (cyw43_backplane_read32(CYW43_ENUM_BASE, &chip_id) == 0){
        uart_puts("CYW43: chip id/rev=");
        uart_puthex(chip_id);
        uart_puts("\n");
    } else{
        uart_puts("CYW43: chip id read failed\n");
        return -1;
    }

    uart_puts("CYW43: uploading firmware bytes=");
    uart_putdec(fw_len);
    uart_puts("\n");

    // Circle clears the last RAM word before the download, writes firmware at
    // RAM base, then places packed NVRAM near the RAM top and writes a length
    // token into the final word.
    if (cyw43_backplane_write(CYW43_RAM_BASE + CYW43_RAM_SIZE - 4u, zero_buf, sizeof(zero_buf)) != 0){
        uart_puts("CYW43: RAM token clear failed\n");
        return -1;
    }

    if (cyw43_backplane_write(CYW43_RAM_BASE, fw_bin, fw_len) != 0){
        uart_puts("CYW43: firmware upload failed\n");
        return -1;
    }

    nvram_addr = CYW43_RAM_BASE + CYW43_RAM_SIZE - nvram_packed_len - 4u;
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
    if (cyw43_backplane_write(CYW43_RAM_BASE + CYW43_RAM_SIZE - 4u, token_buf, sizeof(token_buf)) != 0){
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
    g_cyw43.iface_up = 1;
    return 0;
}

int cyw43_ioctl_down(void){
    g_cyw43.iface_up = 0;
    g_cyw43.joined = 0;
    g_cyw43.joined_ssid[0] = 0;
    return 0;
}

int cyw43_ioctl_scan(cyw43_scan_result_t* out, unsigned int cap, unsigned int* out_count){
    if (!out_count){
        return -1;
    }
    if (!g_cyw43.iface_up){
        *out_count = 0;
        return -1;
    }

    // Phase-1 scaffold: report one synthetic AP entry so shell flow and
    // userspace integration can be validated before full event path lands.
    if (out && cap > 0u){
        unsigned int i = 0;
        const char* demo = "QOS-LAB";
        for (i = 0; i < 32u && demo[i]; i++){
            out[0].ssid[i] = demo[i];
        }
        out[0].ssid[i] = 0;
        out[0].channel = 6;
        out[0].rssi_dbm = -42;
        out[0].auth = 3; // WPA2-PSK style marker in this scaffold.
        *out_count = 1u;
    } else{
        *out_count = 0u;
    }

    g_cyw43.last_scan_count = *out_count;
    return 0;
}

int cyw43_ioctl_join(const char* ssid, const char* password){
    unsigned int n = 0;
    (void)password;

    if (!ssid || !*ssid || !g_cyw43.iface_up){
        return -1;
    }
    n = strn_len_local(ssid, 32u);
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
