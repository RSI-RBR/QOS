#include "cyw43.h"
#include "sdio_bus.h"
#include "fat32.h"
#include "blockdev.h"
#include "memory.h"
#include "uart.h"

#define CYW43_FW_MAX_BYTES      (768u * 1024u)
#define CYW43_NVRAM_MAX_BYTES   (16u * 1024u)
#define CYW43_NVRAM_PACKED_MAX  (20u * 1024u)
#define CYW43_SDIO_XFER_CHUNK   256u

// Minimal backplane RAM staging addresses.
// These are placeholders for phase-1 bring-up and will be refined once
// full backplane window + core reset/clock sequence is implemented.
#define CYW43_FW_STAGE_ADDR     0x00002000u
#define CYW43_NVRAM_STAGE_ADDR  0x000E0000u

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
    *packed_len = w;
    return 0;
}

static int cyw43_write_stage(unsigned int addr, const unsigned char* data, unsigned int len){
    unsigned int off = 0;
    while (off < len){
        unsigned int n = kmin_u32(CYW43_SDIO_XFER_CHUNK, len - off);
        if (sdio_bus_cmd53_write(1, addr + off, &data[off], n) != 0){
            return -1;
        }
        off += n;
    }
    return 0;
}

int cyw43_init(void){
    if (g_cyw43.enabled){
        return 0;
    }

    if (sdio_bus_init() != 0){
        uart_puts("CYW43: SDIO init failed\n");
        return -1;
    }

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

    // Phase-1 uploader: copy firmware + packed NVRAM to staged RAM addresses.
    // Full backplane core reset/clock and verify sequence comes next.
    if (cyw43_write_stage(CYW43_FW_STAGE_ADDR, fw_bin, fw_len) != 0){
        uart_puts("CYW43: firmware upload failed\n");
        return -1;
    }
    if (cyw43_write_stage(CYW43_NVRAM_STAGE_ADDR, nvram_packed, nvram_packed_len) != 0){
        uart_puts("CYW43: NVRAM upload failed\n");
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

    if (blockdev_reinit() != 0){
        uart_puts("CYW43: storage reinit failed\n");
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
