#include "syscall.h"

#define MAX_APS 64u
#define RAW_BUF_BYTES 2304u
#define SSID_MAX 32u
#define DEFAULT_SCAN_CHANNEL 6u
#define STALE_RECOVER_SECS 3u

typedef struct {
    unsigned char bssid[6];
    char ssid[SSID_MAX + 1u];
    unsigned char ssid_len;
    unsigned char channel;
    unsigned char seen;
    unsigned char privacy;
    unsigned char rsn;
    unsigned char wpa;
    int sig_min;
    int sig_max;
    unsigned int beacons;
    unsigned int probe_resps;
    unsigned int clients;
    unsigned char client_macs[8][6];
} ap_info_t;

static ap_info_t g_aps[MAX_APS];

static void put_u32(unsigned int v){
    char tmp[16];
    int n = 0;
    if (v == 0u){
        qos_putc('0');
        return;
    }
    while (v > 0u && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0){
        qos_putc(tmp[--n]);
    }
}

static void put_i32(int v){
    if (v < 0){
        qos_putc('-');
        put_u32((unsigned int)(-(long long)v));
        return;
    }
    put_u32((unsigned int)v);
}

static void put_hex8(unsigned int v){
    static const char h[] = "0123456789ABCDEF";
    qos_putc(h[(v >> 4u) & 0xFu]);
    qos_putc(h[v & 0xFu]);
}

static void put_mac(const unsigned char mac[6]){
    for (unsigned int i = 0; i < 6u; i++){
        if (i){
            qos_putc(':');
        }
        put_hex8(mac[i]);
    }
}

static unsigned int le16(const unsigned char* p){
    return ((unsigned int)p[0]) | ((unsigned int)p[1] << 8);
}

static int mac_eq(const unsigned char a[6], const unsigned char b[6]){
    unsigned char diff = 0u;
    for (unsigned int i = 0; i < 6u; i++){
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0u;
}

static void mac_copy(unsigned char dst[6], const unsigned char src[6]){
    for (unsigned int i = 0; i < 6u; i++){
        dst[i] = src[i];
    }
}

static int mac_is_broadcast(const unsigned char mac[6]){
    for (unsigned int i = 0; i < 6u; i++){
        if (mac[i] != 0xFFu){
            return 0;
        }
    }
    return 1;
}

static int ssid_char_ok(unsigned char c){
    return c >= 32u && c <= 126u;
}

static int find_ap(const unsigned char bssid[6]){
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (g_aps[i].seen && mac_eq(g_aps[i].bssid, bssid)){
            return (int)i;
        }
    }
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (!g_aps[i].seen){
            g_aps[i].seen = 1u;
            mac_copy(g_aps[i].bssid, bssid);
            g_aps[i].sig_min = 127;
            g_aps[i].sig_max = -127;
            return (int)i;
        }
    }
    return -1;
}

static void note_signal(ap_info_t* ap, int sig){
    if (!ap){
        return;
    }
    if (sig < ap->sig_min){
        ap->sig_min = sig;
    }
    if (sig > ap->sig_max){
        ap->sig_max = sig;
    }
}

static void note_client(ap_info_t* ap, const unsigned char mac[6]){
    if (!ap || !mac || mac_is_broadcast(mac)){
        return;
    }
    for (unsigned int i = 0; i < ap->clients && i < 8u; i++){
        if (mac_eq(ap->client_macs[i], mac)){
            return;
        }
    }
    if (ap->clients < 8u){
        mac_copy(ap->client_macs[ap->clients], mac);
    }
    ap->clients++;
}

static void set_ssid(ap_info_t* ap, const unsigned char* ssid, unsigned int len){
    if (!ap || !ssid || len == 0u || ap->ssid_len != 0u){
        return;
    }
    if (len > SSID_MAX){
        len = SSID_MAX;
    }
    for (unsigned int i = 0; i < len; i++){
        ap->ssid[i] = ssid_char_ok(ssid[i]) ? (char)ssid[i] : '?';
    }
    ap->ssid[len] = 0;
    ap->ssid_len = (unsigned char)len;
}

static void parse_tags(ap_info_t* ap, const unsigned char* p, unsigned int len){
    unsigned int off = 0u;
    while (off + 2u <= len){
        unsigned int id = p[off];
        unsigned int n = p[off + 1u];
        const unsigned char* v = p + off + 2u;
        if (off + 2u + n > len){
            break;
        }
        if (id == 0u){
            set_ssid(ap, v, n);
        } else if (id == 3u && n >= 1u){
            ap->channel = v[0];
        } else if (id == 48u){
            ap->rsn = 1u;
        } else if (id == 221u && n >= 4u &&
                   v[0] == 0x00u && v[1] == 0x50u && v[2] == 0xF2u && v[3] == 0x01u){
            ap->wpa = 1u;
        }
        off += 2u + n;
    }
}

static int is_open_ap(const ap_info_t* ap){
    return ap && ap->seen && ap->ssid_len != 0u && !ap->privacy && !ap->rsn && !ap->wpa;
}

static void print_ap_line(const ap_info_t* ap, unsigned int idx){
    qos_puts("AP ");
    put_u32(idx);
    qos_puts(" ");
    put_mac(ap->bssid);
    qos_puts(" ch=");
    put_u32(ap->channel);
    qos_puts(" sig=");
    put_i32(ap->sig_min);
    qos_puts("..");
    put_i32(ap->sig_max);
    qos_puts(" enc=");
    qos_puts(is_open_ap(ap) ? "OPEN" : "SEC");
    qos_puts(" ssid=\"");
    qos_puts(ap->ssid_len ? ap->ssid : "<hidden>");
    qos_puts("\" clients=");
    put_u32(ap->clients);
    qos_puts(" bcn=");
    put_u32(ap->beacons);
    qos_puts(" pr=");
    put_u32(ap->probe_resps);
    qos_puts("\n");
}

static void print_summary(unsigned int frames, unsigned int rt, unsigned int beacon,
                          unsigned int probe_req, unsigned int probe_resp,
                          unsigned int data, unsigned int eapol, unsigned int bad){
    unsigned int aps = 0u;
    unsigned int open = 0u;
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (g_aps[i].seen){
            aps++;
            if (is_open_ap(&g_aps[i])){
                open++;
            }
        }
    }
    qos_puts("scan frames=");
    put_u32(frames);
    qos_puts(" rt=");
    put_u32(rt);
    qos_puts(" aps=");
    put_u32(aps);
    qos_puts(" open=");
    put_u32(open);
    qos_puts(" beacon=");
    put_u32(beacon);
    qos_puts(" probe_req=");
    put_u32(probe_req);
    qos_puts(" probe_resp=");
    put_u32(probe_resp);
    qos_puts(" data=");
    put_u32(data);
    qos_puts(" eapol=");
    put_u32(eapol);
    qos_puts(" bad=");
    put_u32(bad);
    qos_puts("\n");
}

static void print_raw_status_line(const char* label){
    cyw43_raw_capture_status_t st;
    if (qos_wifi_raw_status(&st) != 0){
        qos_puts(label);
        qos_puts(": raw status failed\n");
        return;
    }
    qos_puts(label);
    qos_puts(": raw=");
    qos_puts(st.enabled ? "on" : "off");
    qos_puts(" queued=");
    put_u32(st.queued);
    qos_puts(" rx=");
    put_u32(st.rx_frames);
    qos_puts(" drop=");
    put_u32(st.dropped);
    qos_puts(" rt=");
    put_u32(st.radiotap_frames);
    qos_puts(" dot11=");
    put_u32(st.dot11_frames);
    qos_puts(" eth=");
    put_u32(st.ethernet_frames);
    qos_puts(" unk=");
    put_u32(st.unknown_frames);
    qos_puts("\n");
}

static void print_monitor_status_line(void){
    cyw43_monitor_status_t st;
    if (qos_wifi_monitor_status(&st) != 0){
        qos_puts("monitor: status failed\n");
        return;
    }
    qos_puts("monitor: ");
    qos_puts(st.enabled ? "on" : "off");
    qos_puts(" mode=");
    put_u32(st.requested_mode);
    qos_puts(" ch=");
    put_u32(st.channel);
    qos_puts(" raw=");
    put_u32(st.raw_enabled);
    qos_puts(" rc=");
    put_i32(st.last_rc);
    qos_puts("\n");
}

static void scanner_ensure_monitor_ready(unsigned int channel){
    cyw43_monitor_status_t st;
    unsigned int use_ch = channel ? channel : DEFAULT_SCAN_CHANNEL;

    (void)qos_wifi_up_monitor();
    if (qos_wifi_monitor_status(&st) == 0){
        if (st.channel != 0u){
            use_ch = st.channel;
        }
        if (st.enabled && st.raw_enabled){
            return;
        }
    }
    (void)qos_wifi_monitor_set(2u, use_ch);
}

static void print_final_aps(void){
    unsigned int idx = 0u;
    qos_puts("\nOpen access points:\n");
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (is_open_ap(&g_aps[i])){
            print_ap_line(&g_aps[i], idx++);
        }
    }
    if (idx == 0u){
        qos_puts(" none\n");
    }

    qos_puts("\nAll APs:\n");
    idx = 0u;
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (g_aps[i].seen){
            print_ap_line(&g_aps[i], idx++);
        }
    }
    if (idx == 0u){
        qos_puts(" none\n");
    }
}

static void parse_frame(const unsigned char* buf, unsigned int len,
                        unsigned int* rt_count, unsigned int* beacon_count,
                        unsigned int* probe_req_count, unsigned int* probe_resp_count,
                        unsigned int* data_count, unsigned int* eapol_count,
                        unsigned int* bad_count){
    const unsigned char* dot = buf;
    unsigned int dot_len = len;
    unsigned int rt_len = 0u;
    int signal = 0;
    unsigned int fc = 0u;
    unsigned int type = 0u;
    unsigned int subtype = 0u;
    unsigned int hdr_len = 24u;
    const unsigned char* addr1 = 0;
    const unsigned char* addr2 = 0;
    const unsigned char* addr3 = 0;

    if (!buf || len < 8u){
        (*bad_count)++;
        return;
    }

    if (buf[0] == 0u && buf[1] == 0u){
        rt_len = le16(buf + 2u);
        if (rt_len >= 8u && rt_len < len){
            (*rt_count)++;
            if (rt_len > 22u){
                signal = (signed char)buf[22];
            }
            dot = buf + rt_len;
            dot_len = len - rt_len;
        }
    }

    if (dot_len < 24u){
        (*bad_count)++;
        return;
    }

    fc = le16(dot);
    if ((fc & 0x0003u) != 0u){
        (*bad_count)++;
        return;
    }
    type = (fc >> 2) & 0x3u;
    subtype = (fc >> 4) & 0xFu;
    addr1 = dot + 4u;
    addr2 = dot + 10u;
    addr3 = dot + 16u;

    if (type == 0u && (subtype == 8u || subtype == 5u)){
        int idx = find_ap(addr3);
        if (idx < 0 || dot_len < 36u){
            (*bad_count)++;
            return;
        }
        ap_info_t* ap = &g_aps[idx];
        note_signal(ap, signal);
        ap->privacy = (le16(dot + 34u) & 0x0010u) ? 1u : ap->privacy;
        parse_tags(ap, dot + 36u, dot_len - 36u);
        if (subtype == 8u){
            ap->beacons++;
            (*beacon_count)++;
        } else{
            ap->probe_resps++;
            (*probe_resp_count)++;
        }
        (void)addr2;
        return;
    }

    if (type == 0u && subtype == 4u){
        (*probe_req_count)++;
        return;
    }

    if (type == 2u){
        unsigned int qos = (subtype & 0x8u) ? 1u : 0u;
        unsigned int to_ds = (fc >> 8) & 1u;
        unsigned int from_ds = (fc >> 9) & 1u;
        unsigned int llc = 0u;
        const unsigned char* bssid = addr3;
        const unsigned char* client = addr2;
        (*data_count)++;
        if (to_ds && !from_ds){
            bssid = addr1;
            client = addr2;
        } else if (!to_ds && from_ds){
            bssid = addr2;
            client = addr1;
        } else if (to_ds && from_ds){
            hdr_len += 6u;
            bssid = addr3;
            client = addr2;
        }
        if (qos){
            hdr_len += 2u;
        }
        if (!mac_is_broadcast(bssid)){
            int idx = find_ap(bssid);
            if (idx >= 0){
                note_client(&g_aps[idx], client);
                note_signal(&g_aps[idx], signal);
            }
        }
        llc = hdr_len;
        if (dot_len >= llc + 8u &&
            dot[llc + 0u] == 0xAAu &&
            dot[llc + 1u] == 0xAAu &&
            dot[llc + 2u] == 0x03u &&
            dot[llc + 6u] == 0x88u &&
            dot[llc + 7u] == 0x8Eu){
            (*eapol_count)++;
        }
        return;
    }

    (*bad_count)++;
}

void program_main(void){
    unsigned char buf[RAW_BUF_BYTES];
    unsigned int frames = 0u;
    unsigned int rt = 0u;
    unsigned int beacon = 0u;
    unsigned int probe_req = 0u;
    unsigned int probe_resp = 0u;
    unsigned int data = 0u;
    unsigned int eapol = 0u;
    unsigned int bad = 0u;
    unsigned long long start = qos_get_time_us();
    unsigned long long next_print = start + 1000000ull;
    unsigned int stale_secs = 0u;
    unsigned int last_rx_frames = 0u;
    unsigned int active_channel = DEFAULT_SCAN_CHANNEL;

    qos_puts("QOS WiFi scanner starting. Use wifimon on <channel> first.\n");
    scanner_ensure_monitor_ready(DEFAULT_SCAN_CHANNEL);
    cyw43_monitor_status_t mon;
    int have_mon = (qos_wifi_monitor_status(&mon) == 0) ? 1 : 0;
    if (have_mon && mon.channel != 0u){
        active_channel = mon.channel;
    }
    print_monitor_status_line();
    if (!have_mon || !mon.raw_enabled){
        int raw_rc = qos_wifi_raw_set_enabled(1u);
        qos_puts("scanner: raw enable rc=");
        put_i32(raw_rc);
        qos_puts("\n");
    } else{
        qos_puts("scanner: raw already enabled; preserving queue\n");
    }
    print_raw_status_line("scanner start");
    {
        cyw43_raw_capture_status_t st0;
        if (qos_wifi_raw_status(&st0) == 0){
            last_rx_frames = st0.rx_frames;
        }
    }

    while (1){
        int n = qos_wifi_raw_recv(buf, sizeof(buf));
        if (n < 0){
            qos_puts("scanner: raw recv failed\n");
            break;
        }
        if (n > 0){
            frames++;
            parse_frame(buf, (unsigned int)n, &rt, &beacon, &probe_req,
                        &probe_resp, &data, &eapol, &bad);
        } else{
            qos_sleep(1u);
        }

        if ((long long)(qos_get_time_us() - next_print) >= 0){
            cyw43_raw_capture_status_t st1;
            print_summary(frames, rt, beacon, probe_req, probe_resp, data, eapol, bad);
            print_raw_status_line("scanner raw");
            if (qos_wifi_raw_status(&st1) == 0){
                if (st1.rx_frames == last_rx_frames){
                    stale_secs++;
                } else{
                    stale_secs = 0u;
                    last_rx_frames = st1.rx_frames;
                }
                if (stale_secs >= STALE_RECOVER_SECS){
                    qos_puts("scanner: rx stalled; rearming monitor\n");
                    scanner_ensure_monitor_ready(active_channel);
                    if (qos_wifi_monitor_status(&mon) == 0 && mon.channel != 0u){
                        active_channel = mon.channel;
                    }
                    stale_secs = 0u;
                }
            }
            next_print += 1000000ull;
        }
    }

    print_summary(frames, rt, beacon, probe_req, probe_resp, data, eapol, bad);
    print_final_aps();
    qos_puts("scanner done.\n");
}
