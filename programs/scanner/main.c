#include "syscall.h"

#define MAX_APS 64u
#define RAW_BUF_BYTES 2304u
#define SSID_MAX 32u
#define MAX_CLIENTS_TRACKED 16u
#define DEFAULT_SCAN_CHANNEL 6u
#define STALE_RECOVER_SECS 8u
#define HOP_DWELL_MS 100u
#define HOP_DWELL_BEACON_ONLY_MS 350u
#define DETAIL_PRINT_SECS 12u
#define KEY_EVENT_RING 96u
#define FULL_REARM_COOLDOWN_SECS 5u

typedef enum {
    CAT_BEACON = 0,
    CAT_DATA,
    CAT_PROBE_REQUEST,
    CAT_PROBE_RESPONSE,
    CAT_KEY,
    CAT_ACTION,
    CAT_BLOCK_ACK,
    CAT_AUTH_REQUEST,
    CAT_QOS_NULL,
    CAT_CLEAR_TO_SEND,
    CAT_ACKNOWLEDGEMENT,
    CAT_UNKNOWN,
    CAT_COUNT
} frame_cat_t;

typedef struct {
    unsigned int frame_counts[CAT_COUNT];
    unsigned int frame_bytes[CAT_COUNT];
    unsigned int frames;
    unsigned int rt;
    unsigned int bad;
    unsigned int hop_ok;
    unsigned int hop_fail;
    unsigned int handshake_hits;
} scan_stats_t;

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
    unsigned int clients;
    unsigned char client_macs[MAX_CLIENTS_TRACKED][6];
    unsigned int data_frames;
    unsigned int key_frames;
    unsigned int handshake_hits;
    unsigned int eapol_msg[5];
    unsigned char hs_m1;
    unsigned char hs_m2;
    unsigned char hs_m3;
    unsigned int cat_counts[CAT_COUNT];
    unsigned int cat_bytes[CAT_COUNT];
} ap_info_t;

typedef struct {
    unsigned char valid;
    unsigned char channel;
    unsigned char msg;
    unsigned short key_info;
    unsigned short key_data_len;
    unsigned char bssid[6];
    unsigned char transmitter[6];
    unsigned char mic[16];
    unsigned char nonce[32];
} key_event_t;

static ap_info_t g_aps[MAX_APS];
static key_event_t g_key_events[KEY_EVENT_RING];
static unsigned int g_key_event_head = 0u;
static unsigned int g_key_event_count = 0u;
static unsigned long long g_last_open_hit_notify_us = 0ull;

/*
 * Include 12/13 so scanners in regions using those channels can still detect
 * local APs. Channel 14 is intentionally excluded (Japan-only, 11b specific).
 */
static const unsigned char g_hop_channels[] = {
    1u, 6u, 11u, 2u, 7u, 12u, 3u, 8u, 13u, 4u, 9u, 5u, 10u
};

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

static void put_hex16(unsigned int v){
    put_hex8((v >> 8u) & 0xFFu);
    put_hex8(v & 0xFFu);
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

static unsigned int be16(const unsigned char* p){
    return ((unsigned int)p[0] << 8) | ((unsigned int)p[1]);
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

static void copy_bytes(unsigned char* dst, const unsigned char* src, unsigned int n){
    if (!dst || !src){
        return;
    }
    for (unsigned int i = 0; i < n; i++){
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

static void note_cat(scan_stats_t* stats, frame_cat_t cat, unsigned int bytes){
    if (!stats || cat >= CAT_COUNT){
        return;
    }
    stats->frame_counts[cat]++;
    stats->frame_bytes[cat] += bytes;
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
    for (unsigned int i = 0; i < ap->clients && i < MAX_CLIENTS_TRACKED; i++){
        if (mac_eq(ap->client_macs[i], mac)){
            return;
        }
    }
    if (ap->clients < MAX_CLIENTS_TRACKED){
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

static const char* ap_security_label(const ap_info_t* ap){
    if (!ap){
        return "UNK";
    }
    if (!ap->privacy && !ap->rsn && !ap->wpa){
        return "OPEN";
    }
    if (ap->rsn && ap->wpa){
        return "RSN+WPA";
    }
    if (ap->rsn){
        return "RSN/WPA2";
    }
    if (ap->wpa){
        return "WPA";
    }
    return "WEP?";
}

static int is_open_ap(const ap_info_t* ap){
    return ap && ap->seen && !ap->privacy && !ap->rsn && !ap->wpa;
}

static void maybe_notify_open_hit(const ap_info_t* ap){
    unsigned long long now_us;
    if (!is_open_ap(ap)){
        return;
    }
    now_us = qos_get_time_us();
    if (g_last_open_hit_notify_us != 0ull &&
        (long long)(now_us - g_last_open_hit_notify_us) < 100000ll){
        return;
    }
    g_last_open_hit_notify_us = now_us;
    (void)qos_headless_open_hit();
}

static void ap_note_cat(ap_info_t* ap, frame_cat_t cat, unsigned int bytes){
    if (!ap || cat >= CAT_COUNT){
        return;
    }
    ap->cat_counts[cat]++;
    ap->cat_bytes[cat] += bytes;
}

static void key_event_push(unsigned int channel,
                           unsigned int msg,
                           unsigned int key_info,
                           unsigned int key_data_len,
                           const unsigned char bssid[6],
                           const unsigned char tx[6],
                           const unsigned char* mic,
                           const unsigned char* nonce){
    key_event_t* ev = &g_key_events[g_key_event_head];
    ev->valid = 1u;
    ev->channel = (unsigned char)channel;
    ev->msg = (unsigned char)msg;
    ev->key_info = (unsigned short)(key_info & 0xFFFFu);
    ev->key_data_len = (unsigned short)(key_data_len & 0xFFFFu);
    copy_bytes(ev->bssid, bssid, 6u);
    copy_bytes(ev->transmitter, tx, 6u);
    if (mic){
        copy_bytes(ev->mic, mic, 16u);
    } else{
        for (unsigned int i = 0; i < 16u; i++){
            ev->mic[i] = 0u;
        }
    }
    if (nonce){
        copy_bytes(ev->nonce, nonce, 32u);
    } else{
        for (unsigned int i = 0; i < 32u; i++){
            ev->nonce[i] = 0u;
        }
    }

    g_key_event_head = (g_key_event_head + 1u) % KEY_EVENT_RING;
    if (g_key_event_count < KEY_EVENT_RING){
        g_key_event_count++;
    }
}

static unsigned int eapol_guess_msg(unsigned int key_info){
    unsigned int ack = (key_info >> 7) & 1u;
    unsigned int mic = (key_info >> 8) & 1u;
    unsigned int install = (key_info >> 6) & 1u;
    unsigned int secure = (key_info >> 9) & 1u;

    if (ack && !mic){
        return 1u;
    }
    if (!ack && mic && !secure){
        return 2u;
    }
    if (ack && mic && install){
        return 3u;
    }
    if (!ack && mic && secure){
        return 4u;
    }
    return 0u;
}

static unsigned int parse_eapol_key(const unsigned char* eap,
                                    unsigned int eap_len,
                                    unsigned int* out_key_info,
                                    unsigned int* out_key_data_len,
                                    const unsigned char** out_nonce,
                                    const unsigned char** out_mic){
    if (!eap || eap_len < 4u){
        return 0u;
    }
    if (eap[1] != 3u){
        return 0u;
    }
    if (eap_len < 4u + 95u){
        return 0u;
    }

    const unsigned char* desc = eap + 4u;
    unsigned int key_info = be16(desc + 1u);
    unsigned int key_data_len = be16(desc + 93u);
    if (out_key_info){
        *out_key_info = key_info;
    }
    if (out_key_data_len){
        *out_key_data_len = key_data_len;
    }
    if (out_nonce){
        *out_nonce = desc + 13u;
    }
    if (out_mic){
        *out_mic = desc + 77u;
    }
    return eapol_guess_msg(key_info);
}

static void ap_note_handshake(ap_info_t* ap, unsigned int msg, scan_stats_t* stats){
    if (!ap){
        return;
    }
    if (msg >= 1u && msg <= 4u){
        ap->eapol_msg[msg]++;
    }
    if (msg == 1u){
        ap->hs_m1 = 1u;
        ap->hs_m2 = 0u;
        ap->hs_m3 = 0u;
        return;
    }
    if (msg == 2u && ap->hs_m1){
        ap->hs_m2 = 1u;
        return;
    }
    if (msg == 3u && ap->hs_m2){
        ap->hs_m3 = 1u;
        return;
    }
    if (msg == 4u && ap->hs_m1 && ap->hs_m2){
        ap->handshake_hits++;
        if (stats){
            stats->handshake_hits++;
        }
        ap->hs_m1 = 0u;
        ap->hs_m2 = 0u;
        ap->hs_m3 = 0u;
    }
}

static void print_cat_line(const char* label, const unsigned int* v){
    qos_puts(label);
    qos_puts(" bcn=");
    put_u32(v[CAT_BEACON]);
    qos_puts(" data=");
    put_u32(v[CAT_DATA]);
    qos_puts(" preq=");
    put_u32(v[CAT_PROBE_REQUEST]);
    qos_puts(" presp=");
    put_u32(v[CAT_PROBE_RESPONSE]);
    qos_puts(" key=");
    put_u32(v[CAT_KEY]);
    qos_puts(" act=");
    put_u32(v[CAT_ACTION]);
    qos_puts(" blk=");
    put_u32(v[CAT_BLOCK_ACK]);
    qos_puts(" auth=");
    put_u32(v[CAT_AUTH_REQUEST]);
    qos_puts(" qnull=");
    put_u32(v[CAT_QOS_NULL]);
    qos_puts(" cts=");
    put_u32(v[CAT_CLEAR_TO_SEND]);
    qos_puts(" ack=");
    put_u32(v[CAT_ACKNOWLEDGEMENT]);
    qos_puts(" unk=");
    put_u32(v[CAT_UNKNOWN]);
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
    qos_puts(ap_security_label(ap));
    qos_puts(" ssid=\"");
    qos_puts(ap->ssid_len ? ap->ssid : "<hidden>");
    qos_puts("\" clients=");
    put_u32(ap->clients);
    qos_puts(" bcn=");
    put_u32(ap->cat_counts[CAT_BEACON]);
    qos_puts(" data=");
    put_u32(ap->data_frames);
    qos_puts(" key=");
    put_u32(ap->key_frames);
    qos_puts(" hs=");
    put_u32(ap->handshake_hits);
    qos_puts("\n");
}

static void print_recent_key_events(void){
    if (g_key_event_count == 0u){
        qos_puts("key events: none\n");
        return;
    }
    qos_puts("key events: ");
    put_u32(g_key_event_count);
    qos_puts(" saved, latest:\n");
    unsigned int show = (g_key_event_count > 6u) ? 6u : g_key_event_count;
    for (unsigned int i = 0u; i < show; i++){
        unsigned int pos = (g_key_event_head + KEY_EVENT_RING - 1u - i) % KEY_EVENT_RING;
        key_event_t* ev = &g_key_events[pos];
        if (!ev->valid){
            continue;
        }
        qos_puts(" key ch=");
        put_u32(ev->channel);
        qos_puts(" msg=");
        put_u32(ev->msg);
        qos_puts(" kinfo=0x");
        put_hex16(ev->key_info);
        qos_puts(" klen=");
        put_u32(ev->key_data_len);
        qos_puts(" bssid=");
        put_mac(ev->bssid);
        qos_puts(" tx=");
        put_mac(ev->transmitter);
        qos_puts(" mic=");
        put_hex8(ev->mic[0]);
        put_hex8(ev->mic[1]);
        put_hex8(ev->mic[2]);
        put_hex8(ev->mic[3]);
        qos_puts("...\n");
    }
}

static void print_summary(const scan_stats_t* st, unsigned int active_channel){
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
    qos_puts("scan ch=");
    put_u32(active_channel);
    qos_puts(" frames=");
    put_u32(st->frames);
    qos_puts(" rt=");
    put_u32(st->rt);
    qos_puts(" aps=");
    put_u32(aps);
    qos_puts(" open=");
    put_u32(open);
    qos_puts(" hs=");
    put_u32(st->handshake_hits);
    qos_puts(" bad=");
    put_u32(st->bad);
    qos_puts(" hop=");
    put_u32(st->hop_ok);
    qos_puts("/");
    put_u32(st->hop_fail);
    qos_puts("\n");

    print_cat_line("counts", st->frame_counts);
    print_cat_line("bytes ", st->frame_bytes);
}

static void print_overview(void){
    unsigned int open_idx = 0u;
    unsigned int all_idx = 0u;
    qos_puts("open access points:\n");
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (is_open_ap(&g_aps[i])){
            print_ap_line(&g_aps[i], open_idx++);
        }
    }
    if (open_idx == 0u){
        qos_puts(" none\n");
    }

    qos_puts("all tracked access points:\n");
    for (unsigned int i = 0; i < MAX_APS; i++){
        if (g_aps[i].seen){
            print_ap_line(&g_aps[i], all_idx++);
        }
    }
    if (all_idx == 0u){
        qos_puts(" none\n");
    }
    print_recent_key_events();
}

static void scanner_ensure_monitor_ready(unsigned int channel){
    cyw43_monitor_status_t st;
    unsigned int use_ch = channel ? channel : DEFAULT_SCAN_CHANNEL;
    static unsigned long long next_full_rearm_us = 0ull;
    unsigned long long now_us = qos_get_time_us();
    int have_st = (qos_wifi_monitor_status(&st) == 0) ? 1 : 0;

    if (have_st){
        if (st.channel != 0u && channel == 0u){
            use_ch = st.channel;
        }
        if (st.enabled && st.raw_enabled){
            if (use_ch != 0u && st.channel != use_ch){
                (void)qos_wifi_monitor_set(2u, use_ch);
            }
            return;
        }
    }

    // Try lightweight monitor re-arm first.
    if (qos_wifi_monitor_set(2u, use_ch) == 0){
        (void)qos_wifi_raw_set_enabled(1u);
        return;
    }

    // Full firmware-side monitor rearm is expensive; throttle it.
    if (next_full_rearm_us != 0ull && (long long)(now_us - next_full_rearm_us) < 0){
        return;
    }
    next_full_rearm_us = now_us + ((unsigned long long)FULL_REARM_COOLDOWN_SECS * 1000000ull);

    (void)qos_wifi_up_monitor();
    (void)qos_wifi_monitor_set(2u, use_ch);
    (void)qos_wifi_raw_set_enabled(1u);
}

static void maybe_hop_channel(unsigned int* active_channel,
                              unsigned int* hop_idx,
                              scan_stats_t* stats){
    if (!active_channel || !hop_idx || !stats){
        return;
    }
    unsigned int idx = *hop_idx;
    idx = (idx + 1u) % (unsigned int)(sizeof(g_hop_channels) / sizeof(g_hop_channels[0]));
    unsigned int ch = (unsigned int)g_hop_channels[idx];
    int rc = qos_wifi_monitor_set(2u, ch);
    if (rc == 0){
        *active_channel = ch;
        *hop_idx = idx;
        stats->hop_ok++;
    } else{
        stats->hop_fail++;
    }
}

static void parse_frame(const unsigned char* buf,
                        unsigned int len,
                        unsigned int active_channel,
                        scan_stats_t* stats){
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

    if (!buf || !stats || len < 8u){
        if (stats){
            stats->bad++;
            note_cat(stats, CAT_UNKNOWN, len);
        }
        return;
    }

    stats->frames++;

    if (buf[0] == 0u && buf[1] == 0u){
        rt_len = le16(buf + 2u);
        if (rt_len >= 8u && rt_len < len){
            stats->rt++;
            if (rt_len > 22u){
                signal = (signed char)buf[22];
            }
            dot = buf + rt_len;
            dot_len = len - rt_len;
        }
    }

    if (dot_len < 24u){
        stats->bad++;
        note_cat(stats, CAT_UNKNOWN, len);
        return;
    }

    fc = le16(dot);
    if ((fc & 0x0003u) != 0u){
        stats->bad++;
        note_cat(stats, CAT_UNKNOWN, len);
        return;
    }

    type = (fc >> 2) & 0x3u;
    subtype = (fc >> 4) & 0xFu;
    addr1 = dot + 4u;
    addr2 = dot + 10u;
    addr3 = dot + 16u;

    if (type == 0u){
        if (subtype == 8u || subtype == 5u){
            int idx = find_ap(addr3);
            note_cat(stats, (subtype == 8u) ? CAT_BEACON : CAT_PROBE_RESPONSE, len);
            if (idx >= 0 && dot_len >= 36u){
                ap_info_t* ap = &g_aps[idx];
                ap_note_cat(ap, (subtype == 8u) ? CAT_BEACON : CAT_PROBE_RESPONSE, len);
                note_signal(ap, signal);
                ap->channel = (unsigned char)active_channel;
                ap->privacy = (le16(dot + 34u) & 0x0010u) ? 1u : ap->privacy;
                parse_tags(ap, dot + 36u, dot_len - 36u);
                maybe_notify_open_hit(ap);
            }
            return;
        }
        if (subtype == 4u){
            note_cat(stats, CAT_PROBE_REQUEST, len);
            return;
        }
        if (subtype == 11u){
            note_cat(stats, CAT_AUTH_REQUEST, len);
            return;
        }
        if (subtype == 13u){
            note_cat(stats, CAT_ACTION, len);
            return;
        }
        note_cat(stats, CAT_UNKNOWN, len);
        return;
    }

    if (type == 1u){
        if (subtype == 9u){
            note_cat(stats, CAT_BLOCK_ACK, len);
            return;
        }
        if (subtype == 12u){
            note_cat(stats, CAT_CLEAR_TO_SEND, len);
            return;
        }
        if (subtype == 13u){
            note_cat(stats, CAT_ACKNOWLEDGEMENT, len);
            return;
        }
        note_cat(stats, CAT_UNKNOWN, len);
        return;
    }

    if (type == 2u){
        unsigned int qos = (subtype & 0x8u) ? 1u : 0u;
        unsigned int to_ds = (fc >> 8) & 1u;
        unsigned int from_ds = (fc >> 9) & 1u;
        unsigned int llc = 0u;
        const unsigned char* bssid = addr3;
        const unsigned char* client = addr2;

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

        int ap_idx = -1;
        if (!mac_is_broadcast(bssid)){
            ap_idx = find_ap(bssid);
            if (ap_idx >= 0){
                ap_info_t* ap = &g_aps[ap_idx];
                note_client(ap, client);
                note_signal(ap, signal);
                ap->channel = (unsigned char)active_channel;
            }
        }

        llc = hdr_len;
        if (dot_len >= llc + 8u &&
            dot[llc + 0u] == 0xAAu &&
            dot[llc + 1u] == 0xAAu &&
            dot[llc + 2u] == 0x03u &&
            dot[llc + 6u] == 0x88u &&
            dot[llc + 7u] == 0x8Eu){
            note_cat(stats, CAT_KEY, len);
            if (ap_idx >= 0){
                ap_info_t* ap = &g_aps[ap_idx];
                ap->key_frames++;
                ap_note_cat(ap, CAT_KEY, len);
            }

            if (dot_len > llc + 8u){
                const unsigned char* eap = dot + llc + 8u;
                unsigned int eap_len = dot_len - (llc + 8u);
                unsigned int key_info = 0u;
                unsigned int key_data_len = 0u;
                const unsigned char* nonce = 0;
                const unsigned char* mic = 0;
                unsigned int msg = parse_eapol_key(eap,
                                                   eap_len,
                                                   &key_info,
                                                   &key_data_len,
                                                   &nonce,
                                                   &mic);
                if (ap_idx >= 0 && msg > 0u){
                    ap_note_handshake(&g_aps[ap_idx], msg, stats);
                }
                key_event_push(active_channel,
                               msg,
                               key_info,
                               key_data_len,
                               bssid,
                               addr2,
                               mic,
                               nonce);
            }
            return;
        }

        if (subtype == 12u){
            note_cat(stats, CAT_QOS_NULL, len);
            if (ap_idx >= 0){
                ap_note_cat(&g_aps[ap_idx], CAT_QOS_NULL, len);
            }
            return;
        }

        note_cat(stats, CAT_DATA, len);
        if (ap_idx >= 0){
            ap_info_t* ap = &g_aps[ap_idx];
            ap->data_frames++;
            ap_note_cat(ap, CAT_DATA, len);
        }
        return;
    }

    note_cat(stats, CAT_UNKNOWN, len);
}

void program_main(void){
    unsigned char buf[RAW_BUF_BYTES];
    scan_stats_t stats;
    unsigned int stale_secs = 0u;
    unsigned int recv_err_streak = 0u;
    unsigned int last_rx_frames = 0u;
    unsigned int active_channel = DEFAULT_SCAN_CHANNEL;
    unsigned int hop_idx = 0u;
    unsigned int hop_dwell_ms = HOP_DWELL_MS;
    unsigned int beacon_only_secs = 0u;
    unsigned int prev_frames = 0u;
    unsigned int prev_non_beacon = 0u;
    unsigned int rearm_attempts = 0u;

    for (unsigned int i = 0; i < CAT_COUNT; i++){
        stats.frame_counts[i] = 0u;
        stats.frame_bytes[i] = 0u;
    }
    stats.frames = 0u;
    stats.rt = 0u;
    stats.bad = 0u;
    stats.hop_ok = 0u;
    stats.hop_fail = 0u;
    stats.handshake_hits = 0u;

    unsigned long long now = qos_get_time_us();
    unsigned long long next_print = now + 2000000ull;
    unsigned long long next_detail = now + ((unsigned long long)DETAIL_PRINT_SECS * 1000000ull);
    unsigned long long next_hop = now + ((unsigned long long)hop_dwell_ms * 1000ull);

    qos_puts("QOS WiFi scanner starting (continuous + channel hop).\n");
    scanner_ensure_monitor_ready(DEFAULT_SCAN_CHANNEL);
    cyw43_monitor_status_t mon;
    int have_mon = (qos_wifi_monitor_status(&mon) == 0) ? 1 : 0;
    if (have_mon && mon.channel != 0u){
        active_channel = mon.channel;
        for (unsigned int i = 0u; i < (unsigned int)(sizeof(g_hop_channels) / sizeof(g_hop_channels[0])); i++){
            if (g_hop_channels[i] == (unsigned char)active_channel){
                hop_idx = i;
                break;
            }
        }
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
        now = qos_get_time_us();
        if ((long long)(now - next_print) >= 0){
            cyw43_raw_capture_status_t st1;
            unsigned int non_beacon = 0u;

            print_summary(&stats, active_channel);
            print_raw_status_line("scanner raw");

            non_beacon = (stats.frames >= stats.frame_counts[CAT_BEACON])
                             ? (stats.frames - stats.frame_counts[CAT_BEACON])
                             : 0u;
            if (stats.frames > prev_frames && non_beacon == prev_non_beacon){
                beacon_only_secs++;
            } else{
                beacon_only_secs = 0u;
            }
            prev_frames = stats.frames;
            prev_non_beacon = non_beacon;

            /*
             * If we only see beacons while hopping, widen dwell time so we can
             * catch probe/data/EAPOL bursts between beacon intervals.
             */
            if (beacon_only_secs >= 5u && hop_dwell_ms < HOP_DWELL_BEACON_ONLY_MS){
                hop_dwell_ms = HOP_DWELL_BEACON_ONLY_MS;
                qos_puts("scanner: beacon-only traffic; using slower hop dwell ");
                put_u32(hop_dwell_ms);
                qos_puts("ms\n");
            } else if (beacon_only_secs == 0u && hop_dwell_ms != HOP_DWELL_MS){
                hop_dwell_ms = HOP_DWELL_MS;
                qos_puts("scanner: non-beacon traffic seen; restoring hop dwell ");
                put_u32(hop_dwell_ms);
                qos_puts("ms\n");
            }

            if (qos_wifi_raw_status(&st1) == 0){
                if (st1.rx_frames == last_rx_frames){
                    stale_secs++;
                } else{
                    stale_secs = 0u;
                    last_rx_frames = st1.rx_frames;
                    rearm_attempts = 0u;
                }
                if (stale_secs >= STALE_RECOVER_SECS){
                    if ((rearm_attempts % 4u) == 0u){
                        qos_puts("scanner: rx stalled; rearming monitor\n");
                    }
                    scanner_ensure_monitor_ready(active_channel);
                    if (qos_wifi_monitor_status(&mon) == 0 && mon.channel != 0u){
                        active_channel = mon.channel;
                    }
                    stale_secs = 0u;
                    rearm_attempts++;
                }
            }
            next_print += 2000000ull;
            if ((long long)(now - next_print) >= 0){
                next_print = now + 2000000ull;
            }
        }

        if ((long long)(now - next_detail) >= 0){
            print_overview();
            next_detail += ((unsigned long long)DETAIL_PRINT_SECS * 1000000ull);
            if ((long long)(now - next_detail) >= 0){
                next_detail = now + ((unsigned long long)DETAIL_PRINT_SECS * 1000000ull);
            }
        }

        if ((long long)(now - next_hop) >= 0){
            maybe_hop_channel(&active_channel, &hop_idx, &stats);
            next_hop += ((unsigned long long)hop_dwell_ms * 1000ull);
            if ((long long)(now - next_hop) >= 0){
                next_hop = now + ((unsigned long long)hop_dwell_ms * 1000ull);
            }
        }

        int n = qos_wifi_raw_recv(buf, sizeof(buf));
        if (n < 0){
            recv_err_streak++;
            if ((recv_err_streak & 0x7u) == 1u){
                qos_puts("scanner: raw recv error; rearming monitor\n");
            }
            scanner_ensure_monitor_ready(active_channel);
            qos_sleep(5u);
        } else if (n > 0){
            recv_err_streak = 0u;
            parse_frame(buf, (unsigned int)n, active_channel, &stats);
        } else{
            recv_err_streak = 0u;
            qos_sleep(1u);
        }

        now = qos_get_time_us();
    }

    print_summary(&stats, active_channel);
    print_overview();
    qos_puts("scanner done.\n");
}
