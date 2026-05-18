#include "syscall.h"

#define MAX_APS 64u
#define RAW_BUF_BYTES 2304u
#define SSID_MAX 32u
#define MAX_CLIENTS_TRACKED 16u
#define DEFAULT_SCAN_CHANNEL 6u
#define STALE_RECOVER_SECS 8u
#define SUMMARY_PRINT_SECS 2u
#define HOP_DWELL_MS 100u
#define HOP_DWELL_BEACON_ONLY_MS 350u
#define DETAIL_PRINT_SECS 20u
#define AUTO_FLUSH_SECS 60u
#define KEY_EVENT_RING 96u
#define FULL_REARM_COOLDOWN_SECS 5u
#define RECV_ERR_FORCE_REARM 8u
#define RECOVERY_SETTLE_MS 30u
#define SCAN_LOAD_BUF_BYTES (64u * 1024u)

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
    unsigned char from_ap;
    unsigned short key_info;
    unsigned short key_data_len;
    unsigned char bssid[6];
    unsigned char receiver[6];
    unsigned char transmitter[6];
    unsigned char mic[16];
    unsigned char nonce[32];
} key_event_t;

static ap_info_t g_aps[MAX_APS];
static key_event_t g_key_events[KEY_EVENT_RING];
static unsigned int g_key_event_head = 0u;
static unsigned int g_key_event_count = 0u;
static unsigned long long g_last_open_hit_notify_us = 0ull;
static unsigned char g_scan_load_buf[SCAN_LOAD_BUF_BYTES];

#define SCAN_LOG_LINE_MAX 8192u
static const char SCAN_LOG_COUNTS[] = "counts.log";
static const char SCAN_LOG_APS[] = "aps.log";
static const char SCAN_LOG_HANDSHAKES[] = "handshakes.log";

typedef struct {
    char s[SCAN_LOG_LINE_MAX];
    unsigned int len;
} scan_log_line_t;

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

static void log_init(scan_log_line_t* l){
    if (l){
        l->len = 0u;
    }
}

static void log_ch(scan_log_line_t* l, char c){
    if (!l || l->len + 1u >= SCAN_LOG_LINE_MAX){
        return;
    }
    l->s[l->len++] = c;
}

static void log_str(scan_log_line_t* l, const char* s){
    if (!l || !s){
        return;
    }
    while (*s){
        log_ch(l, *s++);
    }
}

static void log_u32(scan_log_line_t* l, unsigned int v){
    char tmp[16];
    int n = 0;
    if (v == 0u){
        log_ch(l, '0');
        return;
    }
    while (v > 0u && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0){
        log_ch(l, tmp[--n]);
    }
}

static void log_u64(scan_log_line_t* l, unsigned long long v){
    char tmp[24];
    int n = 0;
    if (v == 0ull){
        log_ch(l, '0');
        return;
    }
    while (v > 0ull && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (unsigned int)(v % 10ull));
        v /= 10ull;
    }
    while (n > 0){
        log_ch(l, tmp[--n]);
    }
}

static void log_i32(scan_log_line_t* l, int v){
    if (v < 0){
        log_ch(l, '-');
        log_u32(l, (unsigned int)(-(long long)v));
        return;
    }
    log_u32(l, (unsigned int)v);
}

static void log_hex8(scan_log_line_t* l, unsigned int v){
    static const char h[] = "0123456789ABCDEF";
    log_ch(l, h[(v >> 4u) & 0xFu]);
    log_ch(l, h[v & 0xFu]);
}

static void log_hex16(scan_log_line_t* l, unsigned int v){
    log_hex8(l, (v >> 8u) & 0xFFu);
    log_hex8(l, v & 0xFFu);
}

static void log_mac(scan_log_line_t* l, const unsigned char mac[6]){
    for (unsigned int i = 0; i < 6u; i++){
        if (i){
            log_ch(l, ':');
        }
        log_hex8(l, mac[i]);
    }
}

static void log_hex_bytes(scan_log_line_t* l, const unsigned char* p, unsigned int n){
    if (!p){
        return;
    }
    for (unsigned int i = 0u; i < n; i++){
        log_hex8(l, p[i]);
    }
}

static void log_ssid(scan_log_line_t* l, const ap_info_t* ap){
    log_ch(l, '"');
    if (!ap || ap->ssid_len == 0u){
        log_str(l, "<hidden>");
    } else{
        for (unsigned int i = 0u; i < ap->ssid_len; i++){
            char c = ap->ssid[i];
            if (c == '"' || c == '\\'){
                log_ch(l, '\\');
                log_ch(l, c);
            } else if ((unsigned char)c >= 32u && (unsigned char)c <= 126u){
                log_ch(l, c);
            } else{
                log_ch(l, '?');
            }
        }
    }
    log_ch(l, '"');
}

static void log_append_line(const char* path, scan_log_line_t* l){
    if (!path || !l || l->len == 0u){
        return;
    }
    (void)qos_file_append_data(path, (const unsigned char*)l->s, l->len);
}

static int flush_scanner_logs(int verbose){
    int ok = 1;
    int rc_counts = qos_scanner_log_flush(SCAN_LOG_COUNTS);
    int rc_aps = qos_scanner_log_flush(SCAN_LOG_APS);
    int rc_hs = qos_scanner_log_flush(SCAN_LOG_HANDSHAKES);

    if (rc_counts != 0 || rc_aps != 0 || rc_hs != 0){
        ok = 0;
    }
    if (verbose || !ok){
        qos_puts("scanner autosave: counts=");
        put_i32(rc_counts);
        qos_puts(" aps=");
        put_i32(rc_aps);
        qos_puts(" handshakes=");
        put_i32(rc_hs);
        qos_puts(ok ? " OK\n" : " failed\n");
    }
    return ok ? 0 : -1;
}

static void print_log_sizes(void){
    int counts = qos_file_size(SCAN_LOG_COUNTS);
    int aps = qos_file_size(SCAN_LOG_APS);
    int handshakes = qos_file_size(SCAN_LOG_HANDSHAKES);
    qos_puts("scanner log bytes: counts_log=");
    put_i32(counts);
    qos_puts(" aps_log=");
    put_i32(aps);
    qos_puts(" handshakes_log=");
    put_i32(handshakes);
    qos_puts(" key_events_ram=");
    put_u32(g_key_event_count);
    qos_puts("\n");
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
    if (!ap || !ap->seen){
        return 0;
    }
    /*
     * Data-only frames do not prove an AP is open; they only prove traffic.
     * Require at least one beacon/probe-response classification before using
     * the absence of privacy/RSN/WPA as an "open" signal.
     */
    if ((ap->cat_counts[CAT_BEACON] + ap->cat_counts[CAT_PROBE_RESPONSE]) == 0u){
        return 0;
    }
    return (!ap->privacy && !ap->rsn && !ap->wpa) ? 1 : 0;
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

static void count_aps(unsigned int* out_aps, unsigned int* out_open){
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
    if (out_aps){
        *out_aps = aps;
    }
    if (out_open){
        *out_open = open;
    }
}

static void log_scanner_start(void){
    scan_log_line_t l;
    unsigned long long now = qos_get_time_us();
    log_init(&l);
    log_str(&l, "QOSSCAN_HANDSHAKES v=2 mode=append fields=t_us,ch,msg,dir,key_info,key_data_len,bssid,rx,tx,mic,nonce,key_data,eapol,raw\n");
    log_append_line(SCAN_LOG_HANDSHAKES, &l);
    log_init(&l);
    log_str(&l, "session t_us=");
    log_u64(&l, now);
    log_str(&l, " mode=monitor-hop\n");
    log_append_line(SCAN_LOG_HANDSHAKES, &l);
}

static void log_summary_line(const scan_stats_t* st, unsigned int active_channel){
    unsigned int aps = 0u;
    unsigned int open = 0u;
    scan_log_line_t l;
    if (!st){
        return;
    }
    count_aps(&aps, &open);
    (void)qos_file_clear(SCAN_LOG_COUNTS);
    log_init(&l);
    log_str(&l, "QOSSCAN_COUNTS v=2 mode=replace fields=total\n");
    log_append_line(SCAN_LOG_COUNTS, &l);

    log_init(&l);
    log_str(&l, "total t_us=");
    log_u64(&l, qos_get_time_us());
    log_str(&l, " ch=");
    log_u32(&l, active_channel);
    log_str(&l, " frames=");
    log_u32(&l, st->frames);
    log_str(&l, " rt=");
    log_u32(&l, st->rt);
    log_str(&l, " aps=");
    log_u32(&l, aps);
    log_str(&l, " open=");
    log_u32(&l, open);
    log_str(&l, " hs=");
    log_u32(&l, st->handshake_hits);
    log_str(&l, " bad=");
    log_u32(&l, st->bad);
    log_str(&l, " hop_ok=");
    log_u32(&l, st->hop_ok);
    log_str(&l, " hop_fail=");
    log_u32(&l, st->hop_fail);
    log_str(&l, " bcn=");
    log_u32(&l, st->frame_counts[CAT_BEACON]);
    log_str(&l, " data=");
    log_u32(&l, st->frame_counts[CAT_DATA]);
    log_str(&l, " preq=");
    log_u32(&l, st->frame_counts[CAT_PROBE_REQUEST]);
    log_str(&l, " presp=");
    log_u32(&l, st->frame_counts[CAT_PROBE_RESPONSE]);
    log_str(&l, " key=");
    log_u32(&l, st->frame_counts[CAT_KEY]);
    log_str(&l, " action=");
    log_u32(&l, st->frame_counts[CAT_ACTION]);
    log_str(&l, " block_ack=");
    log_u32(&l, st->frame_counts[CAT_BLOCK_ACK]);
    log_str(&l, " auth=");
    log_u32(&l, st->frame_counts[CAT_AUTH_REQUEST]);
    log_str(&l, " qos_null=");
    log_u32(&l, st->frame_counts[CAT_QOS_NULL]);
    log_str(&l, " cts=");
    log_u32(&l, st->frame_counts[CAT_CLEAR_TO_SEND]);
    log_str(&l, " ack=");
    log_u32(&l, st->frame_counts[CAT_ACKNOWLEDGEMENT]);
    log_str(&l, " unk=");
    log_u32(&l, st->frame_counts[CAT_UNKNOWN]);
    log_ch(&l, '\n');
    log_append_line(SCAN_LOG_COUNTS, &l);
}

static void log_ap_line(const ap_info_t* ap){
    scan_log_line_t l;
    if (!ap || !ap->seen){
        return;
    }
    log_init(&l);
    log_str(&l, "t_us=");
    log_u64(&l, qos_get_time_us());
    log_str(&l, " bssid=");
    log_mac(&l, ap->bssid);
    log_str(&l, " ch=");
    log_u32(&l, ap->channel);
    log_str(&l, " sig_min=");
    log_i32(&l, ap->sig_min);
    log_str(&l, " sig_max=");
    log_i32(&l, ap->sig_max);
    log_str(&l, " enc=");
    log_str(&l, ap_security_label(ap));
    log_str(&l, " ssid=");
    log_ssid(&l, ap);
    log_str(&l, " clients=");
    log_u32(&l, ap->clients);
    log_str(&l, " bcn=");
    log_u32(&l, ap->cat_counts[CAT_BEACON]);
    log_str(&l, " data=");
    log_u32(&l, ap->data_frames);
    log_str(&l, " key=");
    log_u32(&l, ap->key_frames);
    log_str(&l, " hs=");
    log_u32(&l, ap->handshake_hits);
    log_ch(&l, '\n');
    log_append_line(SCAN_LOG_APS, &l);
}

static void log_ap_snapshot(void){
    scan_log_line_t l;
    (void)qos_file_clear(SCAN_LOG_APS);
    log_init(&l);
    log_str(&l, "QOSSCAN_APS v=2 mode=replace key=bssid fields=ap\n");
    log_append_line(SCAN_LOG_APS, &l);
    for (unsigned int i = 0u; i < MAX_APS; i++){
        if (g_aps[i].seen){
            log_ap_line(&g_aps[i]);
        }
    }
}

static const char* find_text(const char* s, const char* needle){
    if (!s || !needle || !needle[0]){
        return 0;
    }
    for (const char* p = s; *p; p++){
        const char* a = p;
        const char* b = needle;
        while (*a && *b && *a == *b){
            a++;
            b++;
        }
        if (*b == 0){
            return p;
        }
    }
    return 0;
}

static unsigned int text_len(const char* s){
    unsigned int n = 0u;
    if (!s){
        return 0u;
    }
    while (s[n]){
        n++;
    }
    return n;
}

static int hex_val(char c){
    if (c >= '0' && c <= '9'){
        return c - '0';
    }
    if (c >= 'a' && c <= 'f'){
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F'){
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_u32_at(const char* p, unsigned int* out){
    unsigned int v = 0u;
    unsigned int any = 0u;
    if (!p || !out){
        return 0;
    }
    while (*p >= '0' && *p <= '9'){
        unsigned int d = (unsigned int)(*p - '0');
        if (v > (0xFFFFFFFFu - d) / 10u){
            return 0;
        }
        v = (v * 10u) + d;
        any = 1u;
        p++;
    }
    if (!any){
        return 0;
    }
    *out = v;
    return 1;
}

static int parse_i32_at(const char* p, int* out){
    unsigned int neg = 0u;
    unsigned int uv = 0u;
    if (!p || !out){
        return 0;
    }
    if (*p == '-'){
        neg = 1u;
        p++;
    }
    if (!parse_u32_at(p, &uv)){
        return 0;
    }
    if (neg){
        if (uv > 2147483648u){
            return 0;
        }
        *out = -(int)uv;
    } else{
        if (uv > 2147483647u){
            return 0;
        }
        *out = (int)uv;
    }
    return 1;
}

static int line_get_u32(const char* line, const char* key, unsigned int* out){
    const char* p = find_text(line, key);
    if (!p){
        return 0;
    }
    return parse_u32_at(p + text_len(key), out);
}

static int line_get_i32(const char* line, const char* key, int* out){
    const char* p = find_text(line, key);
    if (!p){
        return 0;
    }
    return parse_i32_at(p + text_len(key), out);
}

static int line_get_mac(const char* line, const char* key, unsigned char mac[6]){
    const char* p = find_text(line, key);
    if (!p || !mac){
        return 0;
    }
    p += text_len(key);
    for (unsigned int i = 0u; i < 6u; i++){
        int hi = hex_val(p[0]);
        int lo = hex_val(p[1]);
        if (hi < 0 || lo < 0){
            return 0;
        }
        mac[i] = (unsigned char)((hi << 4) | lo);
        p += 2;
        if (i < 5u){
            if (*p != ':'){
                return 0;
            }
            p++;
        }
    }
    return 1;
}

static unsigned int copy_quoted_ssid_from_line(const char* line,
                                               char out[SSID_MAX + 1u]){
    const char* p = find_text(line, "ssid=\"");
    unsigned int n = 0u;
    if (!p || !out){
        return 0u;
    }
    p += 6;
    if (p[0] == '<' && find_text(p, "<hidden>") == p){
        out[0] = 0;
        return 0u;
    }
    while (*p && *p != '"' && n < SSID_MAX){
        if (*p == '\\' && p[1]){
            p++;
        }
        out[n++] = ssid_char_ok((unsigned char)*p) ? *p : '?';
        p++;
    }
    out[n] = 0;
    return n;
}

static void parse_counts_line_into_stats(const char* line, scan_stats_t* st){
    unsigned int v;
    if (!line || !st || !find_text(line, "frames=")){
        return;
    }
    if (line_get_u32(line, "frames=", &v)) st->frames = v;
    if (line_get_u32(line, "rt=", &v)) st->rt = v;
    if (line_get_u32(line, "hs=", &v)) st->handshake_hits = v;
    if (line_get_u32(line, "bad=", &v)) st->bad = v;
    if (line_get_u32(line, "hop_ok=", &v)) st->hop_ok = v;
    if (line_get_u32(line, "hop_fail=", &v)) st->hop_fail = v;
    if (line_get_u32(line, "bcn=", &v)) st->frame_counts[CAT_BEACON] = v;
    if (line_get_u32(line, "data=", &v)) st->frame_counts[CAT_DATA] = v;
    if (line_get_u32(line, "preq=", &v)) st->frame_counts[CAT_PROBE_REQUEST] = v;
    if (line_get_u32(line, "presp=", &v)) st->frame_counts[CAT_PROBE_RESPONSE] = v;
    if (line_get_u32(line, "key=", &v)) st->frame_counts[CAT_KEY] = v;
    if (line_get_u32(line, "action=", &v)) st->frame_counts[CAT_ACTION] = v;
    if (line_get_u32(line, "block_ack=", &v)) st->frame_counts[CAT_BLOCK_ACK] = v;
    if (line_get_u32(line, "auth=", &v)) st->frame_counts[CAT_AUTH_REQUEST] = v;
    if (line_get_u32(line, "qos_null=", &v)) st->frame_counts[CAT_QOS_NULL] = v;
    if (line_get_u32(line, "cts=", &v)) st->frame_counts[CAT_CLEAR_TO_SEND] = v;
    if (line_get_u32(line, "ack=", &v)) st->frame_counts[CAT_ACKNOWLEDGEMENT] = v;
    if (line_get_u32(line, "unk=", &v)) st->frame_counts[CAT_UNKNOWN] = v;
}

static unsigned int load_counts_from_fat(scan_stats_t* st){
    int n;
    char* line;
    if (!st){
        return 0u;
    }
    n = qos_scanner_log_read_fat(SCAN_LOG_COUNTS, 0u, g_scan_load_buf, SCAN_LOAD_BUF_BYTES - 1u);
    if (n <= 0){
        return 0u;
    }
    g_scan_load_buf[n] = 0u;
    line = (char*)g_scan_load_buf;
    for (int i = 0; i <= n; i++){
        if (g_scan_load_buf[i] == '\r' || g_scan_load_buf[i] == '\n' || g_scan_load_buf[i] == 0u){
            char saved = (char)g_scan_load_buf[i];
            g_scan_load_buf[i] = 0u;
            parse_counts_line_into_stats(line, st);
            if (saved == 0){
                break;
            }
            line = (char*)&g_scan_load_buf[i + 1];
        }
    }
    return (unsigned int)n;
}

static void parse_ap_line_into_table(const char* line, unsigned int* loaded){
    unsigned char mac[6];
    unsigned int uv;
    int iv;
    char ssid[SSID_MAX + 1u];
    if (!line || !line_get_mac(line, "bssid=", mac)){
        return;
    }
    int idx = find_ap(mac);
    if (idx < 0){
        return;
    }
    ap_info_t* ap = &g_aps[idx];
    if (line_get_u32(line, "ch=", &uv) && uv <= 255u){
        ap->channel = (unsigned char)uv;
    }
    if (line_get_i32(line, "sig_min=", &iv)){
        if (ap->sig_min == 127 || iv < ap->sig_min){
            ap->sig_min = iv;
        }
    }
    if (line_get_i32(line, "sig_max=", &iv)){
        if (ap->sig_max == -127 || iv > ap->sig_max){
            ap->sig_max = iv;
        }
    }
    if (find_text(line, "enc=RSN")){
        ap->privacy = 1u;
        ap->rsn = 1u;
    } else if (find_text(line, "enc=WPA")){
        ap->privacy = 1u;
        ap->wpa = 1u;
    } else if (find_text(line, "enc=WEP")){
        ap->privacy = 1u;
    }
    uv = copy_quoted_ssid_from_line(line, ssid);
    if (uv > 0u && ap->ssid_len == 0u){
        set_ssid(ap, (const unsigned char*)ssid, uv);
    }
    if (line_get_u32(line, "clients=", &uv) && uv > ap->clients){
        ap->clients = uv;
    }
    if (line_get_u32(line, "bcn=", &uv) && uv > ap->cat_counts[CAT_BEACON]){
        ap->cat_counts[CAT_BEACON] = uv;
    }
    if (line_get_u32(line, "data=", &uv) && uv > ap->data_frames){
        ap->data_frames = uv;
        ap->cat_counts[CAT_DATA] = uv;
    }
    if (line_get_u32(line, "key=", &uv) && uv > ap->key_frames){
        ap->key_frames = uv;
        ap->cat_counts[CAT_KEY] = uv;
    }
    if (line_get_u32(line, "hs=", &uv) && uv > ap->handshake_hits){
        ap->handshake_hits = uv;
    }
    if (loaded){
        (*loaded)++;
    }
}

static unsigned int load_aps_from_fat(void){
    int n;
    char* line;
    unsigned int loaded = 0u;
    n = qos_scanner_log_read_fat(SCAN_LOG_APS, 0u, g_scan_load_buf, SCAN_LOAD_BUF_BYTES - 1u);
    if (n <= 0){
        return 0u;
    }
    g_scan_load_buf[n] = 0u;
    line = (char*)g_scan_load_buf;
    for (int i = 0; i <= n; i++){
        if (g_scan_load_buf[i] == '\r' || g_scan_load_buf[i] == '\n' || g_scan_load_buf[i] == 0u){
            char saved = (char)g_scan_load_buf[i];
            g_scan_load_buf[i] = 0u;
            parse_ap_line_into_table(line, &loaded);
            if (saved == 0){
                break;
            }
            line = (char*)&g_scan_load_buf[i + 1];
        }
    }
    return loaded;
}

static void load_persistent_scanner_state(scan_stats_t* st){
    unsigned int count_bytes = load_counts_from_fat(st);
    unsigned int ap_lines = load_aps_from_fat();
    if (count_bytes || ap_lines){
        qos_puts("scanner loaded prior state: counts_bytes=");
        put_u32(count_bytes);
        qos_puts(" ap_lines=");
        put_u32(ap_lines);
        qos_puts("\n");
    }
}

static void log_key_event_line(const key_event_t* ev,
                               const unsigned char* key_data,
                               const unsigned char* eapol,
                               unsigned int eapol_len,
                               const unsigned char* raw,
                               unsigned int raw_len){
    scan_log_line_t l;
    if (!ev || !ev->valid){
        return;
    }
    log_init(&l);
    log_str(&l, "hs t_us=");
    log_u64(&l, qos_get_time_us());
    log_str(&l, " ch=");
    log_u32(&l, ev->channel);
    log_str(&l, " msg=");
    log_u32(&l, ev->msg);
    log_str(&l, " dir=");
    log_str(&l, ev->from_ap ? "ap_to_client" : "client_to_ap");
    log_str(&l, " key_info=0x");
    log_hex16(&l, ev->key_info);
    log_str(&l, " key_data_len=");
    log_u32(&l, ev->key_data_len);
    log_str(&l, " bssid=");
    log_mac(&l, ev->bssid);
    log_str(&l, " rx=");
    log_mac(&l, ev->receiver);
    log_str(&l, " tx=");
    log_mac(&l, ev->transmitter);
    log_str(&l, " mic=");
    log_hex_bytes(&l, ev->mic, 16u);
    log_str(&l, " nonce=");
    log_hex_bytes(&l, ev->nonce, 32u);
    log_str(&l, " key_data=");
    log_hex_bytes(&l, key_data, ev->key_data_len);
    log_str(&l, " eapol_len=");
    log_u32(&l, eapol_len);
    log_str(&l, " eapol=");
    log_hex_bytes(&l, eapol, eapol_len);
    log_str(&l, " raw_len=");
    log_u32(&l, raw_len);
    log_str(&l, " raw=");
    log_hex_bytes(&l, raw, raw_len);
    log_ch(&l, '\n');
    log_append_line(SCAN_LOG_HANDSHAKES, &l);
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
                           const unsigned char rx[6],
                           const unsigned char tx[6],
                           const unsigned char* mic,
                           const unsigned char* nonce,
                           const unsigned char* key_data,
                           const unsigned char* eapol,
                           unsigned int eapol_len,
                           const unsigned char* raw,
                           unsigned int raw_len){
    key_event_t* ev = &g_key_events[g_key_event_head];
    ev->valid = 1u;
    ev->channel = (unsigned char)channel;
    ev->msg = (unsigned char)msg;
    ev->from_ap = (bssid && tx && mac_eq(bssid, tx)) ? 1u : 0u;
    ev->key_info = (unsigned short)(key_info & 0xFFFFu);
    ev->key_data_len = (unsigned short)(key_data_len & 0xFFFFu);
    copy_bytes(ev->bssid, bssid, 6u);
    copy_bytes(ev->receiver, rx, 6u);
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
    log_key_event_line(ev, key_data, eapol, eapol_len, raw, raw_len);
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
                                    const unsigned char** out_key_data,
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
    unsigned int key_data_avail = (eap_len > 99u) ? (eap_len - 99u) : 0u;
    if (key_data_len > key_data_avail){
        key_data_len = key_data_avail;
    }
    if (out_key_info){
        *out_key_info = key_info;
    }
    if (out_key_data_len){
        *out_key_data_len = key_data_len;
    }
    if (out_key_data){
        *out_key_data = desc + 95u;
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

static int scanner_note_raw_progress(const cyw43_raw_capture_status_t* st,
                                     unsigned int* last_rx_frames){
    int progressed = 0;
    if (!st || !last_rx_frames){
        return 0;
    }

    /*
     * Raw capture reset clears the firmware-side counter to zero. Treat only a
     * forward-moving counter or queued frames as progress; a backward jump just
     * means the recovery path reset the queue, not that RX came back.
     */
    if (st->rx_frames > *last_rx_frames || st->queued > 0u){
        progressed = 1;
    }
    *last_rx_frames = st->rx_frames;
    return progressed;
}

static int scanner_wait_for_raw_progress(unsigned int* last_rx_frames,
                                         unsigned int total_wait_ms){
    cyw43_raw_capture_status_t st;
    unsigned int waited = 0u;
    unsigned int step = RECOVERY_SETTLE_MS;
    if (step == 0u){
        step = 1u;
    }
    while (waited <= total_wait_ms){
        if (qos_wifi_raw_status(&st) == 0 &&
            scanner_note_raw_progress(&st, last_rx_frames)){
            return 1;
        }
        if (waited >= total_wait_ms){
            break;
        }
        qos_sleep(step);
        waited += step;
    }
    return 0;
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
        qos_puts(" dir=");
        qos_puts(ev->from_ap ? "ap" : "client");
        qos_puts(" bssid=");
        put_mac(ev->bssid);
        qos_puts(" rx=");
        put_mac(ev->receiver);
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
    count_aps(&aps, &open);
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
    log_ap_snapshot();
    print_log_sizes();
}

static void scanner_ensure_monitor_ready(unsigned int channel, unsigned int force_rearm){
    cyw43_monitor_status_t st;
    unsigned int use_ch = channel ? channel : DEFAULT_SCAN_CHANNEL;
    static unsigned long long next_full_rearm_us = 0ull;
    unsigned long long now_us = qos_get_time_us();
    int have_st = (qos_wifi_monitor_status(&st) == 0) ? 1 : 0;

    if (have_st){
        if (st.channel != 0u && channel == 0u){
            use_ch = st.channel;
        }
        if (!force_rearm && st.enabled && st.raw_enabled){
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
    if (!force_rearm &&
        next_full_rearm_us != 0ull && (long long)(now_us - next_full_rearm_us) < 0){
        return;
    }
    next_full_rearm_us = now_us + ((unsigned long long)FULL_REARM_COOLDOWN_SECS * 1000000ull);

    (void)qos_wifi_up_monitor();
    (void)qos_wifi_monitor_set(2u, use_ch);
    (void)qos_wifi_raw_set_enabled(1u);
}

static int scanner_restore_monitor_path(unsigned int channel){
    cyw43_monitor_status_t st;
    unsigned int ch = channel ? channel : DEFAULT_SCAN_CHANNEL;
    int recover_rc = -1;
    for (unsigned int attempt = 0u; attempt < 2u; attempt++){
        int rc = qos_wifi_up_monitor();
        if (rc == 0){
            scanner_ensure_monitor_ready(ch, 1u);
            if (qos_wifi_monitor_status(&st) == 0 && st.enabled && st.raw_enabled){
                return 1;
            }
        }
        qos_sleep(40u);
        if (qos_wifi_monitor_status(&st) == 0 && st.enabled && st.raw_enabled){
            return 1;
        }
    }

    qos_puts("scanner: monitor restore failed; hard recovery\n");
    recover_rc = qos_wifi_monitor_recover(ch);
    if (recover_rc == 0 &&
        qos_wifi_monitor_status(&st) == 0 && st.enabled && st.raw_enabled){
        return 1;
    }
    qos_puts("scanner: hard recovery rc=");
    put_i32(recover_rc);
    qos_puts("\n");
    return 0;
}

static int scanner_recover_rx_stall(unsigned int active_channel,
                                    unsigned int* stage,
                                    unsigned int* last_rx_frames){
    unsigned int ch = active_channel ? active_channel : DEFAULT_SCAN_CHANNEL;
    unsigned int s = stage ? *stage : 0u;
    int recovered = 0;

    if (s == 0u){
        qos_puts("scanner: rx stalled; raw/monitor rearm\n");
        (void)qos_wifi_raw_set_enabled(0u);
        qos_sleep(RECOVERY_SETTLE_MS);
        (void)qos_wifi_monitor_set(2u, ch);
        (void)qos_wifi_raw_set_enabled(1u);
        recovered = scanner_wait_for_raw_progress(last_rx_frames, 250u);
        if (stage){
            *stage = recovered ? 0u : 1u;
        }
        return recovered;
    }

    if (s == 1u){
        qos_puts("scanner: rx stalled; force monitor reapply\n");
        (void)qos_wifi_raw_set_enabled(0u);
        (void)qos_wifi_monitor_set(0u, 0u);
        qos_sleep(RECOVERY_SETTLE_MS);
        (void)qos_wifi_monitor_set(2u, ch);
        (void)qos_wifi_raw_set_enabled(1u);
        recovered = scanner_wait_for_raw_progress(last_rx_frames, 400u);
        if (stage){
            *stage = recovered ? 0u : 2u;
        }
        return recovered;
    }

    if (s == 2u){
        qos_puts("scanner: rx stalled; full monitor up/reapply\n");
        (void)qos_wifi_raw_set_enabled(0u);
    (void)scanner_restore_monitor_path(ch);
    recovered = scanner_wait_for_raw_progress(last_rx_frames, 650u);
        if (stage){
            *stage = recovered ? 0u : 3u;
        }
        return recovered;
    }

    qos_puts("scanner: rx stalled; hard WiFi monitor recovery\n");
    (void)qos_wifi_raw_set_enabled(0u);
    if (last_rx_frames){
        *last_rx_frames = 0u;
    }
    if (!scanner_restore_monitor_path(ch)){
        if (stage){
            *stage = 3u;
        }
        return 0;
    }
    recovered = scanner_wait_for_raw_progress(last_rx_frames, 2000u);
    if (stage){
        *stage = recovered ? 0u : 1u;
    }
    return recovered;
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
                maybe_notify_open_hit(ap);
            }

            if (dot_len > llc + 8u){
                const unsigned char* eap = dot + llc + 8u;
                unsigned int eap_len = dot_len - (llc + 8u);
                unsigned int key_info = 0u;
                unsigned int key_data_len = 0u;
                const unsigned char* key_data = 0;
                const unsigned char* nonce = 0;
                const unsigned char* mic = 0;
                const unsigned char* eapol = dot + llc;
                unsigned int eapol_len = dot_len - llc;
                unsigned int msg = parse_eapol_key(eap,
                                                   eap_len,
                                                   &key_info,
                                                   &key_data_len,
                                                   &key_data,
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
                               addr1,
                               addr2,
                               mic,
                               nonce,
                               key_data,
                               eapol,
                               eapol_len,
                               buf,
                               len);
            }
            return;
        }

        if (subtype == 12u){
            note_cat(stats, CAT_QOS_NULL, len);
            if (ap_idx >= 0){
                ap_info_t* ap = &g_aps[ap_idx];
                ap_note_cat(ap, CAT_QOS_NULL, len);
                maybe_notify_open_hit(ap);
            }
            return;
        }

        note_cat(stats, CAT_DATA, len);
        if (ap_idx >= 0){
            ap_info_t* ap = &g_aps[ap_idx];
            ap->data_frames++;
            ap_note_cat(ap, CAT_DATA, len);
            maybe_notify_open_hit(ap);
        }
        return;
    }

    note_cat(stats, CAT_UNKNOWN, len);
}

void program_main(void){
    unsigned char buf[RAW_BUF_BYTES];
    scan_stats_t stats;
    unsigned int recv_err_streak = 0u;
    unsigned int last_rx_frames = 0u;
    unsigned int active_channel = DEFAULT_SCAN_CHANNEL;
    unsigned int hop_idx = 0u;
    unsigned int hop_dwell_ms = HOP_DWELL_MS;
    unsigned int beacon_only_secs = 0u;
    unsigned int prev_frames = 0u;
    unsigned int prev_non_beacon = 0u;
    unsigned int rearm_stage = 0u;
    unsigned long long last_rx_progress_us = 0ull;

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

    load_persistent_scanner_state(&stats);

    unsigned long long now = qos_get_time_us();
    last_rx_progress_us = now;
    unsigned long long next_print = now + ((unsigned long long)SUMMARY_PRINT_SECS * 1000000ull);
    unsigned long long next_detail = now + ((unsigned long long)DETAIL_PRINT_SECS * 1000000ull);
    unsigned long long next_hop = now + ((unsigned long long)hop_dwell_ms * 1000ull);
    unsigned long long next_flush = now + ((unsigned long long)AUTO_FLUSH_SECS * 1000000ull);

    qos_puts("QOS WiFi scanner starting (continuous + channel hop).\n");
    qos_puts("scanner logs: counts/aps replace state, handshakes append, FAT autosave 60s\n");
    log_scanner_start();
    log_summary_line(&stats, active_channel);
    log_ap_snapshot();
    /*
     * Always (re)enter monitor minimal-up first for scanner sessions so we
     * start from a clean capture state even if station-mode commands were run
     * earlier in this boot.
     */
    (void)scanner_restore_monitor_path(DEFAULT_SCAN_CHANNEL);
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
            log_summary_line(&stats, active_channel);
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
                if (scanner_note_raw_progress(&st1, &last_rx_frames)){
                    last_rx_progress_us = now;
                    rearm_stage = 0u;
                }
                if (!st1.enabled){
                    qos_puts("scanner: raw capture disabled; rearming monitor\n");
                    scanner_ensure_monitor_ready(active_channel, 1u);
                }
            }
            next_print += ((unsigned long long)SUMMARY_PRINT_SECS * 1000000ull);
            if ((long long)(now - next_print) >= 0){
                next_print = now + ((unsigned long long)SUMMARY_PRINT_SECS * 1000000ull);
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

        if ((long long)(now - next_flush) >= 0){
            log_summary_line(&stats, active_channel);
            log_ap_snapshot();
            (void)flush_scanner_logs(1);
            (void)scanner_restore_monitor_path(active_channel);
            next_flush += ((unsigned long long)AUTO_FLUSH_SECS * 1000000ull);
            if ((long long)(now - next_flush) >= 0){
                next_flush = now + ((unsigned long long)AUTO_FLUSH_SECS * 1000000ull);
            }
        }

        if ((long long)(now - last_rx_progress_us) >= ((long long)STALE_RECOVER_SECS * 1000000ll)){
            int recovered;
            qos_puts("scanner: rx idle window hit; recovering monitor path\n");
            recovered = scanner_recover_rx_stall(active_channel, &rearm_stage, &last_rx_frames);
            if (qos_wifi_monitor_status(&mon) == 0 && mon.channel != 0u){
                active_channel = mon.channel;
            }
            next_hop = now + 1000000ull;
            /*
             * Give each recovery attempt its own settle window, but only reset
             * the stage to zero after actual raw RX progress. Counter resets
             * alone no longer count as health.
             */
            last_rx_progress_us = recovered ? qos_get_time_us() : now;
        }

        int n = qos_wifi_raw_recv(buf, sizeof(buf));
        if (n < 0){
            recv_err_streak++;
            if ((recv_err_streak & 0x1Fu) == 1u){
                qos_puts("scanner: raw recv error; rearming monitor\n");
            }
            /*
             * Avoid hammering full firmware rearm on transient SDIO misses.
             * Escalate only if errors persist for a while.
             */
            scanner_ensure_monitor_ready(active_channel,
                                         (recv_err_streak >= RECV_ERR_FORCE_REARM) ? 1u : 0u);
            qos_sleep(2u);
        } else if (n > 0){
            recv_err_streak = 0u;
            last_rx_progress_us = now;
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
