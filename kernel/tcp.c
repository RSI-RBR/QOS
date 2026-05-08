#include "tcp.h"
#include "ipv4.h"
#include "net_proto.h"
#include "timer.h"
#include "net.h"
#include "uart.h"
#include "memory.h"
#include "crypto.h"
#include "sha256.h"
#include "x25519.h"
#include "tls_handshake.h"
#include "tls_key_schedule.h"
#include "tls_record.h"
#include "x509_verify.h"

typedef struct __attribute__((packed)) {
    unsigned short src_port_be;
    unsigned short dst_port_be;
    unsigned int seq_be;
    unsigned int ack_be;
    unsigned char data_off_flags_hi; // upper nibble=data offset
    unsigned char flags_lo;
    unsigned short window_be;
    unsigned short checksum_be;
    unsigned short urg_ptr_be;
} tcp_header_t;

typedef struct {
    unsigned long rx_total;
    unsigned long rx_match;
    unsigned long rx_data;
    unsigned long rx_drop;
    unsigned long tx_total;
    unsigned long tx_fail;
    unsigned long syn_sent;
    unsigned long synack_rx;
    unsigned long est_ok;
    unsigned long fin_rx;
    unsigned long http_ok;
    unsigned long http_fail;
} tcp_stats_t;

enum {
    TCP_ST_CLOSED = 0,
    TCP_ST_SYN_SENT = 1,
    TCP_ST_ESTABLISHED = 2,
    TCP_ST_CLOSE_WAIT = 3
};

typedef struct {
    int active;
    int state;
    unsigned char peer_ip[4];
    unsigned short peer_port;
    unsigned short local_port;
    unsigned int iss;
    unsigned int snd_una;
    unsigned int snd_nxt;
    unsigned int rcv_nxt;
    unsigned char* out;
    unsigned int out_cap;
    unsigned int out_len;
    unsigned long last_rx_tick;
} tcp_conn_t;

static tcp_stats_t g_tcp_stats;
static tcp_conn_t g_conn;
static unsigned short g_next_local_port = 42000u;
static int g_tcp_https_last_error = 0;
static unsigned long g_tcp_https_fail_count = 0;

#define TCP_RECV_WINDOW_MAX 60000u

#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u

static unsigned short be16_read(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static unsigned long read_cntfrq(void){
    unsigned long v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static unsigned long read_cntpct(void){
    unsigned long v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static void be16_write(unsigned char* p, unsigned short v){
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned int be32_read(const unsigned char* p){
    return ((unsigned int)p[0] << 24) |
           ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) |
           ((unsigned int)p[3]);
}

static void be32_write(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8) & 0xFFu);
    p[3] = (unsigned char)(v & 0xFFu);
}

static int ip4_eq(const unsigned char a[4], const unsigned char b[4]){
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static unsigned long sum_add_bytes(unsigned long sum, const unsigned char* data, unsigned int len){
    unsigned int i = 0;
    while (i + 1u < len){
        sum += ((unsigned long)data[i] << 8) | data[i + 1u];
        i += 2u;
    }
    if (i < len){
        sum += ((unsigned long)data[i] << 8);
    }
    return sum;
}

static unsigned short sum_finalize(unsigned long sum){
    while (sum >> 16){
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (unsigned short)(~sum & 0xFFFFu);
}

static unsigned short tcp_checksum(const unsigned char src_ip[4],
                                   const unsigned char dst_ip[4],
                                   const unsigned char* tcp_bytes,
                                   unsigned int tcp_len){
    unsigned long sum = 0;
    sum = sum_add_bytes(sum, src_ip, 4);
    sum = sum_add_bytes(sum, dst_ip, 4);
    sum += 6u; // TCP
    sum += (unsigned long)tcp_len;
    sum = sum_add_bytes(sum, tcp_bytes, tcp_len);
    return sum_finalize(sum);
}

static unsigned short tcp_advertised_window(void){
    unsigned int room = TCP_RECV_WINDOW_MAX;
    if (g_conn.out && g_conn.out_cap > g_conn.out_len){
        room = g_conn.out_cap - g_conn.out_len;
    }
    if (room > TCP_RECV_WINDOW_MAX){
        room = TCP_RECV_WINDOW_MAX;
    }
    return (unsigned short)room;
}

static int tcp_send_segment(unsigned char flags,
                            const unsigned char* payload,
                            unsigned int payload_len){
    unsigned char local_ip[4];
    unsigned char buf[20 + 1460];
    unsigned int tcp_len = 20u + payload_len;

    if (!g_conn.active || payload_len > 1460u){
        g_tcp_stats.tx_fail++;
        return -1;
    }

    for (unsigned int i = 0; i < tcp_len; i++){
        buf[i] = 0;
    }
    be16_write(&buf[0], g_conn.local_port);
    be16_write(&buf[2], g_conn.peer_port);
    be32_write(&buf[4], g_conn.snd_nxt);
    be32_write(&buf[8], g_conn.rcv_nxt);
    buf[12] = (unsigned char)(5u << 4); // data offset
    buf[13] = flags;
    be16_write(&buf[14], tcp_advertised_window());
    be16_write(&buf[16], 0);
    be16_write(&buf[18], 0);

    for (unsigned int i = 0; i < payload_len; i++){
        buf[20 + i] = payload[i];
    }

    net_proto_get_local_ip(local_ip);
    unsigned short csum = tcp_checksum(local_ip, g_conn.peer_ip, buf, tcp_len);
    if (csum == 0){
        csum = 0xFFFFu;
    }
    be16_write(&buf[16], csum);

    if (ipv4_send_via_gateway(6u, g_conn.peer_ip, buf, tcp_len) != 0){
        g_tcp_stats.tx_fail++;
        return -1;
    }
    g_tcp_stats.tx_total++;

    if (flags & TCP_FLAG_SYN){
        g_conn.snd_nxt += 1u;
    }
    if (flags & TCP_FLAG_FIN){
        g_conn.snd_nxt += 1u;
    }
    g_conn.snd_nxt += payload_len;
    return 0;
}

void tcp_init(void){
    g_tcp_stats.rx_total = 0;
    g_tcp_stats.rx_match = 0;
    g_tcp_stats.rx_data = 0;
    g_tcp_stats.rx_drop = 0;
    g_tcp_stats.tx_total = 0;
    g_tcp_stats.tx_fail = 0;
    g_tcp_stats.syn_sent = 0;
    g_tcp_stats.synack_rx = 0;
    g_tcp_stats.est_ok = 0;
    g_tcp_stats.fin_rx = 0;
    g_tcp_stats.http_ok = 0;
    g_tcp_stats.http_fail = 0;
    g_conn.active = 0;
    g_conn.state = TCP_ST_CLOSED;
}

void tcp_handle_ipv4_packet(const unsigned char src_ip[4],
                            const unsigned char dst_ip[4],
                            const unsigned char* payload,
                            unsigned int payload_len){
    (void)dst_ip;
    g_tcp_stats.rx_total++;
    if (!g_conn.active || !src_ip || !payload || payload_len < 20u){
        g_tcp_stats.rx_drop++;
        return;
    }

    const tcp_header_t* h = (const tcp_header_t*)payload;
    unsigned short src_port = be16_read((const unsigned char*)&h->src_port_be);
    unsigned short dst_port = be16_read((const unsigned char*)&h->dst_port_be);
    unsigned int seq = be32_read((const unsigned char*)&h->seq_be);
    unsigned int ack = be32_read((const unsigned char*)&h->ack_be);
    unsigned int hdr_len = ((unsigned int)(h->data_off_flags_hi >> 4) & 0xFu) * 4u;
    unsigned char flags = h->flags_lo;

    if (hdr_len < 20u || hdr_len > payload_len){
        g_tcp_stats.rx_drop++;
        return;
    }
    if (!ip4_eq(src_ip, g_conn.peer_ip) || src_port != g_conn.peer_port || dst_port != g_conn.local_port){
        g_tcp_stats.rx_drop++;
        return;
    }
    g_tcp_stats.rx_match++;

    if (flags & TCP_FLAG_RST){
        g_conn.state = TCP_ST_CLOSED;
        g_conn.active = 0;
        return;
    }

    if (g_conn.state == TCP_ST_SYN_SENT){
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == g_conn.snd_nxt){
            g_tcp_stats.synack_rx++;
            g_conn.snd_una = ack;
            g_conn.rcv_nxt = seq + 1u;
            if (tcp_send_segment(TCP_FLAG_ACK, 0, 0) == 0){
                g_conn.state = TCP_ST_ESTABLISHED;
                g_tcp_stats.est_ok++;
            }
        }
        return;
    }

    if ((flags & TCP_FLAG_ACK) && ack > g_conn.snd_una && ack <= g_conn.snd_nxt){
        g_conn.snd_una = ack;
    }

    unsigned int data_len = payload_len - hdr_len;
    if (data_len > 0){
        if (seq == g_conn.rcv_nxt){
            unsigned int room = (g_conn.out_len < g_conn.out_cap) ? (g_conn.out_cap - g_conn.out_len) : 0u;
            unsigned int take = (data_len < room) ? data_len : room;
            for (unsigned int i = 0; i < take; i++){
                g_conn.out[g_conn.out_len + i] = payload[hdr_len + i];
            }
            g_conn.out_len += take;
            g_conn.rcv_nxt += data_len;
            g_conn.last_rx_tick = system_ticks;
            g_tcp_stats.rx_data += data_len;
            (void)tcp_send_segment(TCP_FLAG_ACK, 0, 0);
        } else{
            (void)tcp_send_segment(TCP_FLAG_ACK, 0, 0);
        }
    }

    if (flags & TCP_FLAG_FIN){
        g_tcp_stats.fin_rx++;
        if (seq + data_len == g_conn.rcv_nxt){
            g_conn.rcv_nxt += 1u;
        } else{
            g_conn.rcv_nxt = seq + data_len + 1u;
        }
        (void)tcp_send_segment(TCP_FLAG_ACK, 0, 0);
        g_conn.state = TCP_ST_CLOSE_WAIT;
        g_conn.last_rx_tick = system_ticks;
    }
}

static int append_str(char* dst, int cap, int* idx, const char* s){
    while (*s){
        if (*idx >= cap){
            return -1;
        }
        dst[*idx] = *s;
        (*idx)++;
        s++;
    }
    return 0;
}

static unsigned char http_ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int http_header_name_eq(const unsigned char* p, unsigned int len, const char* name){
    unsigned int i = 0u;
    if (!p || !name){
        return 0;
    }
    while (i < len && name[i]){
        if (http_ascii_lower(p[i]) != http_ascii_lower((unsigned char)name[i])){
            return 0;
        }
        i++;
    }
    return i == len && name[i] == 0;
}

static int http_value_has_token(const unsigned char* p, unsigned int len, const char* token){
    unsigned int tlen = 0u;
    if (!p || !token){
        return 0;
    }
    while (token[tlen]){
        tlen++;
    }
    if (tlen == 0u || len < tlen){
        return 0;
    }
    for (unsigned int i = 0u; i + tlen <= len; i++){
        unsigned int j = 0u;
        while (j < tlen && http_ascii_lower(p[i + j]) == http_ascii_lower((unsigned char)token[j])){
            j++;
        }
        if (j == tlen){
            return 1;
        }
    }
    return 0;
}

static int http_find_body_offset(const unsigned char* buf, unsigned int len, unsigned int* out_body){
    if (!buf || !out_body || len < 4u){
        return 0;
    }
    for (unsigned int i = 0u; i + 3u < len; i++){
        if (buf[i] == '\r' && buf[i + 1u] == '\n' &&
            buf[i + 2u] == '\r' && buf[i + 3u] == '\n'){
            *out_body = i + 4u;
            return 1;
        }
    }
    return 0;
}

// Returns 1 when complete, 0 when definitely waiting for more, -1 when
// headers are complete but no exact length is available.
static int http_response_completion_state(const unsigned char* buf, unsigned int len){
    unsigned int body = 0u;
    unsigned int i = 0u;
    unsigned int content_len = 0u;
    int have_content_len = 0;
    int chunked = 0;

    if (!http_find_body_offset(buf, len, &body)){
        return 0;
    }

    while (i < body){
        unsigned int ls = i;
        unsigned int le;
        unsigned int colon = 0xFFFFFFFFu;
        unsigned int vs;
        unsigned int ve;

        while (i < body && buf[i] != '\n'){
            if (buf[i] == ':' && colon == 0xFFFFFFFFu){
                colon = i;
            }
            i++;
        }
        le = i;
        if (i < body && buf[i] == '\n'){
            i++;
        }
        while (le > ls && (buf[le - 1u] == '\r' || buf[le - 1u] == '\n')){
            le--;
        }
        if (le == ls){
            break;
        }
        if (colon == 0xFFFFFFFFu || colon <= ls || colon >= le){
            continue;
        }

        vs = colon + 1u;
        while (vs < le && (buf[vs] == ' ' || buf[vs] == '\t')){
            vs++;
        }
        ve = le;
        while (ve > vs && (buf[ve - 1u] == ' ' || buf[ve - 1u] == '\t')){
            ve--;
        }

        if (http_header_name_eq(&buf[ls], colon - ls, "content-length")){
            unsigned int v = 0u;
            int ok = 0;
            for (unsigned int k = vs; k < ve; k++){
                if (buf[k] < '0' || buf[k] > '9'){
                    ok = 0;
                    break;
                }
                ok = 1;
                v = (v * 10u) + (unsigned int)(buf[k] - '0');
            }
            if (ok){
                content_len = v;
                have_content_len = 1;
            }
        } else if (http_header_name_eq(&buf[ls], colon - ls, "transfer-encoding") &&
                   http_value_has_token(&buf[vs], ve - vs, "chunked")){
            chunked = 1;
        }
    }

    if (have_content_len){
        return ((len - body) >= content_len) ? 1 : 0;
    }
    if (chunked){
        for (unsigned int k = body; k + 4u < len; k++){
            if (buf[k] == '\r' && buf[k + 1u] == '\n' &&
                buf[k + 2u] == '0' &&
                (buf[k + 3u] == '\r' || buf[k + 3u] == ';')){
                return 1;
            }
        }
        return 0;
    }
    return -1;
}

#define TCP_TLS_REC_MAX (16384u + 256u)
#define TCP_TLS_RX_CAP 262144u
#define TCP_TLS_HS_BUF_CAP 32768u
#define TCP_TLS_APP_IO_CAP (TCP_TLS_REC_MAX + 5u)

static unsigned char g_tls_rx_raw[TCP_TLS_RX_CAP];
static unsigned char g_tls_hs_buf[TCP_TLS_HS_BUF_CAP];
static unsigned char g_tls_record_wire[TCP_TLS_APP_IO_CAP];
static unsigned char g_tls_record_plain[TCP_TLS_REC_MAX];
static unsigned char g_tls_record_tx[TCP_TLS_APP_IO_CAP];

// Experimental PQ advertisement scaffolding:
// - Group 0x6399: ecosystem draft ID for X25519+Kyber768 style KEM hybrids.
// - Signature scheme 0x0905: draft allocation for ML-DSA-65 (Dilithium level 3).
// TLS key exchange remains X25519-only until a KEM backend is integrated.
// Keep this OFF by default for broad compatibility with strict servers.
#define TLS13_GROUP_X25519_KYBER768_DRAFT00 0x6399u
#define TLS13_SIGALG_MLDSA65 0x0905u
static const int g_tls13_advertise_pq = 0;

static void be24_write(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)((v >> 16) & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)(v & 0xFFu);
}

static unsigned int be24_read(const unsigned char* p){
    return ((unsigned int)p[0] << 16) |
           ((unsigned int)p[1] << 8) |
           (unsigned int)p[2];
}

static int tls13_build_client_hello_sni_x25519(const char* host,
                                                const unsigned char client_pub[32],
                                                unsigned char* out,
                                                unsigned int out_cap,
                                                unsigned int* out_len){
    if (!host || !*host || !client_pub || !out || !out_len){
        return -1;
    }

    unsigned int host_len = 0;
    while (host[host_len]){
        host_len++;
        if (host_len > 250u){
            return -1;
        }
    }

    unsigned int i = 0;
    out[i++] = (unsigned char)TLS13_HS_TYPE_CLIENT_HELLO;
    out[i++] = 0;
    out[i++] = 0;
    out[i++] = 0;

    if (i + 2u + 32u + 1u + 32u + 2u + 2u + 1u + 1u + 2u > out_cap){
        return -1;
    }

    be16_write(&out[i], TLS13_VERSION_LEGACY); i += 2u;
    if (crypto_random_bytes(&out[i], 32u) != 0){
        for (unsigned int r = 0; r < 32u; r++){
            out[i + r] = (unsigned char)(0xA5u + r);
        }
    }
    i += 32u;

    out[i++] = 32u; // legacy_session_id length (compat mode)
    if (crypto_random_bytes(&out[i], 32u) != 0){
        for (unsigned int r = 0; r < 32u; r++){
            out[i + r] = (unsigned char)(0x5Au + r);
        }
    }
    i += 32u;

    be16_write(&out[i], 2u); i += 2u;
    be16_write(&out[i], TLS13_CIPHER_AES_128_GCM_SHA256); i += 2u;
    out[i++] = 1u; // legacy_compression_methods len
    out[i++] = 0u; // null compression

    unsigned int ext_len_pos = i;
    i += 2u;
    unsigned int ext_start = i;

    // server_name
    {
        unsigned int ext_len = 2u + 1u + 2u + host_len;
        if (i + 4u + ext_len > out_cap){
            return -1;
        }
        be16_write(&out[i], 0x0000u); i += 2u;
        be16_write(&out[i], (unsigned short)ext_len); i += 2u;
        be16_write(&out[i], (unsigned short)(1u + 2u + host_len)); i += 2u; // server_name_list len
        out[i++] = 0u; // host_name
        be16_write(&out[i], (unsigned short)host_len); i += 2u;
        for (unsigned int h = 0; h < host_len; h++){
            out[i++] = (unsigned char)host[h];
        }
    }

    // supported_versions
    if (i + 7u > out_cap){
        return -1;
    }
    be16_write(&out[i], 0x002Bu); i += 2u;
    be16_write(&out[i], 3u); i += 2u;
    out[i++] = 2u;
    be16_write(&out[i], TLS13_VERSION_1_3); i += 2u;

    // supported_groups
    {
        unsigned short groups[2];
        unsigned int gcount = 0;
        groups[gcount++] = TLS13_GROUP_X25519;
        if (g_tls13_advertise_pq){
            groups[gcount++] = TLS13_GROUP_X25519_KYBER768_DRAFT00;
        }
        unsigned int groups_bytes = gcount * 2u;
        unsigned int ext_len = 2u + groups_bytes;
        if (i + 4u + ext_len > out_cap){
            return -1;
        }
        be16_write(&out[i], 0x000Au); i += 2u;
        be16_write(&out[i], (unsigned short)ext_len); i += 2u;
        be16_write(&out[i], (unsigned short)groups_bytes); i += 2u;
        for (unsigned int gi = 0; gi < gcount; gi++){
            be16_write(&out[i], groups[gi]); i += 2u;
        }
    }

    // signature_algorithms
    {
        unsigned short sigs[16];
        unsigned int scount = 0;
        sigs[scount++] = 0x0804u; // rsa_pss_rsae_sha256
        sigs[scount++] = 0x0805u; // rsa_pss_rsae_sha384
        sigs[scount++] = 0x0806u; // rsa_pss_rsae_sha512
        sigs[scount++] = 0x0809u; // rsa_pss_pss_sha256
        sigs[scount++] = 0x080Au; // rsa_pss_pss_sha384
        sigs[scount++] = 0x080Bu; // rsa_pss_pss_sha512
        // Keep broader compatibility for CertificateVerify negotiation.
        // Chain verification remains enforced in x509_verify.
        sigs[scount++] = 0x0403u; // ecdsa_secp256r1_sha256
        sigs[scount++] = 0x0503u; // ecdsa_secp384r1_sha384
        sigs[scount++] = 0x0603u; // ecdsa_secp521r1_sha512
        sigs[scount++] = 0x0807u; // ed25519
        sigs[scount++] = 0x0401u; // rsa_pkcs1_sha256
        sigs[scount++] = 0x0501u; // rsa_pkcs1_sha384
        sigs[scount++] = 0x0601u; // rsa_pkcs1_sha512
        if (g_tls13_advertise_pq){
            sigs[scount++] = TLS13_SIGALG_MLDSA65;
        }
        unsigned int sig_bytes = scount * 2u;
        unsigned int ext_len = 2u + sig_bytes;
        if (i + 4u + ext_len > out_cap){
            return -1;
        }
        be16_write(&out[i], 0x000Du); i += 2u;
        be16_write(&out[i], (unsigned short)ext_len); i += 2u;
        be16_write(&out[i], (unsigned short)sig_bytes); i += 2u;
        for (unsigned int si = 0; si < scount; si++){
            be16_write(&out[i], sigs[si]); i += 2u;
        }
    }

    // signature_algorithms_cert
    {
        unsigned short sigs[16];
        unsigned int scount = 0;
        sigs[scount++] = 0x0804u; // rsa_pss_rsae_sha256
        sigs[scount++] = 0x0805u; // rsa_pss_rsae_sha384
        sigs[scount++] = 0x0806u; // rsa_pss_rsae_sha512
        sigs[scount++] = 0x0809u; // rsa_pss_pss_sha256
        sigs[scount++] = 0x080Au; // rsa_pss_pss_sha384
        sigs[scount++] = 0x080Bu; // rsa_pss_pss_sha512
        // Keep this broad for compatibility: some endpoints abort the
        // handshake if their cert algorithm is not offered here.
        sigs[scount++] = 0x0403u; // ecdsa_secp256r1_sha256
        sigs[scount++] = 0x0503u; // ecdsa_secp384r1_sha384
        sigs[scount++] = 0x0603u; // ecdsa_secp521r1_sha512
        sigs[scount++] = 0x0807u; // ed25519
        sigs[scount++] = 0x0401u; // rsa_pkcs1_sha256
        sigs[scount++] = 0x0501u; // rsa_pkcs1_sha384
        sigs[scount++] = 0x0601u; // rsa_pkcs1_sha512
        if (g_tls13_advertise_pq){
            sigs[scount++] = TLS13_SIGALG_MLDSA65;
        }
        unsigned int sig_bytes = scount * 2u;
        unsigned int ext_len = 2u + sig_bytes;
        if (i + 4u + ext_len > out_cap){
            return -1;
        }
        be16_write(&out[i], 0x0032u); i += 2u;
        be16_write(&out[i], (unsigned short)ext_len); i += 2u;
        be16_write(&out[i], (unsigned short)sig_bytes); i += 2u;
        for (unsigned int si = 0; si < scount; si++){
            be16_write(&out[i], sigs[si]); i += 2u;
        }
    }

    // ALPN: request normal HTTP/1.1 service. Some large HTTPS frontends use
    // ALPN policy to select the protocol stack behind the TLS terminator.
    {
        static const unsigned char alpn_http11[] = {
            0x00u, 0x09u, 0x08u,
            'h', 't', 't', 'p', '/', '1', '.', '1'
        };
        unsigned int ext_len = (unsigned int)sizeof(alpn_http11);
        if (i + 4u + ext_len > out_cap){
            return -1;
        }
        be16_write(&out[i], 0x0010u); i += 2u;
        be16_write(&out[i], (unsigned short)ext_len); i += 2u;
        for (unsigned int a = 0; a < ext_len; a++){
            out[i++] = alpn_http11[a];
        }
    }

    // key_share (x25519 only until PQ KEM encapsulation is implemented)
    if (i + 42u > out_cap){
        return -1;
    }
    be16_write(&out[i], 0x0033u); i += 2u;
    be16_write(&out[i], 38u); i += 2u;
    be16_write(&out[i], 36u); i += 2u;
    be16_write(&out[i], TLS13_GROUP_X25519); i += 2u;
    be16_write(&out[i], 32u); i += 2u;
    for (unsigned int k = 0; k < 32u; k++){
        out[i++] = client_pub[k];
    }

    unsigned int ext_len = i - ext_start;
    if (ext_len > 0xFFFFu){
        return -1;
    }
    be16_write(&out[ext_len_pos], (unsigned short)ext_len);

    unsigned int body_len = i - 4u;
    if (body_len > 0xFFFFFFu){
        return -1;
    }
    be24_write(&out[1], body_len);

    *out_len = i;
    return 0;
}

static int tls13_build_plain_record(unsigned char type,
                                    const unsigned char* fragment,
                                    unsigned int frag_len,
                                    unsigned char* out,
                                    unsigned int out_cap,
                                    unsigned int* out_len){
    if (!out || !out_len || (!fragment && frag_len > 0u)){
        return -1;
    }
    if (frag_len > 0xFFFFu || out_cap < (5u + frag_len)){
        return -1;
    }
    out[0] = type;
    out[1] = 0x03u;
    out[2] = 0x03u;
    out[3] = (unsigned char)((frag_len >> 8) & 0xFFu);
    out[4] = (unsigned char)(frag_len & 0xFFu);
    for (unsigned int i = 0; i < frag_len; i++){
        out[5u + i] = fragment[i];
    }
    *out_len = 5u + frag_len;
    return 0;
}

static int tls13_wait_for_established(void){
    unsigned long start_tick = system_ticks;
    unsigned long start_cnt = read_cntpct();
    unsigned long freq = read_cntfrq();
    unsigned long handshake_to_cnt;
    unsigned long spin_budget = 25000000UL;

    if (freq == 0){
        freq = 1000000UL;
    }
    handshake_to_cnt = (freq / 1000UL) * 2500UL;
    if (handshake_to_cnt == 0){
        handshake_to_cnt = freq;
    }

    while (g_conn.state == TCP_ST_SYN_SENT){
        (void)net_poll();
        if ((read_cntpct() - start_cnt) > handshake_to_cnt){
            return -1;
        }
        if ((long)(system_ticks - start_tick) > 2000){
            return -1;
        }
        if (spin_budget-- == 0){
            return -1;
        }
    }
    return (g_conn.state == TCP_ST_ESTABLISHED) ? 0 : -1;
}

static int tls13_compact_rx(unsigned int* consumed){
    if (!consumed){
        return -1;
    }
    if (*consumed == 0u){
        return 0;
    }
    if (*consumed > g_conn.out_len){
        return -1;
    }
    unsigned int rem = g_conn.out_len - *consumed;
    for (unsigned int i = 0; i < rem; i++){
        g_conn.out[i] = g_conn.out[*consumed + i];
    }
    g_conn.out_len = rem;
    *consumed = 0u;
    return 0;
}

static int tls13_pull_record(unsigned int* consumed,
                             unsigned char* rec_type,
                             const unsigned char** rec_payload,
                             unsigned int* rec_len,
                             unsigned char rec_hdr[5],
                             unsigned int timeout_ms){
    if (!consumed || !rec_type || !rec_payload || !rec_len || !rec_hdr){
        return -1;
    }

    unsigned long start_tick = system_ticks;
    while (1){
        if (*consumed > g_conn.out_len){
            return -1;
        }

        if ((*consumed > 8192u) || (*consumed == g_conn.out_len && *consumed > 0u)){
            if (tls13_compact_rx(consumed) != 0){
                return -1;
            }
        }

        unsigned int avail = g_conn.out_len - *consumed;
        if (avail >= 5u){
            const unsigned char* p = &g_conn.out[*consumed];
            unsigned int body_len = be16_read(&p[3]);
            if (body_len > TCP_TLS_REC_MAX){
                return -1;
            }
            if (avail >= 5u + body_len){
                *rec_type = p[0];
                for (unsigned int h = 0; h < 5u; h++){
                    rec_hdr[h] = p[h];
                }
                *rec_payload = p + 5u;
                *rec_len = body_len;
                *consumed += 5u + body_len;
                return 0;
            }
        }

        if (timeout_ms > 0u && (unsigned long)(system_ticks - start_tick) > timeout_ms){
            return -1;
        }
        if (g_conn.state == TCP_ST_CLOSE_WAIT && avail == 0u){
            return -1;
        }
        (void)net_poll();
    }
}

static void sha256_snapshot(const sha256_ctx_t* in, unsigned char out[32]){
    if (!in || !out){
        return;
    }
    sha256_ctx_t tmp = *in;
    sha256_final(&tmp, out);
}

static int tls13_derive_master_and_app_secrets(const unsigned char shared_secret[32],
                                                const unsigned char transcript_hash_server_finished[32],
                                                unsigned char client_app_secret[32],
                                                unsigned char server_app_secret[32]){
    unsigned char zero[32];
    unsigned char empty_hash[32];
    unsigned char early[32];
    unsigned char d1[32];
    unsigned char hs_secret[32];
    unsigned char d2[32];
    unsigned char master[32];
    int rc = -1;

    for (unsigned int i = 0; i < 32u; i++){
        zero[i] = 0;
    }
    sha256_digest(0, 0u, empty_hash);
    crypto_hkdf_sha256_extract(zero, sizeof(zero), zero, sizeof(zero), early);
    if (tls13_hkdf_expand_label_sha256(early, "derived", empty_hash, sizeof(empty_hash), d1, sizeof(d1)) != 0){
        goto done;
    }
    crypto_hkdf_sha256_extract(d1, sizeof(d1), shared_secret, 32u, hs_secret);
    if (tls13_hkdf_expand_label_sha256(hs_secret, "derived", empty_hash, sizeof(empty_hash), d2, sizeof(d2)) != 0){
        goto done;
    }
    crypto_hkdf_sha256_extract(d2, sizeof(d2), zero, sizeof(zero), master);

    if (tls13_hkdf_expand_label_sha256(master, "c ap traffic",
                                       transcript_hash_server_finished, 32u,
                                       client_app_secret, 32u) != 0){
        goto done;
    }
    if (tls13_hkdf_expand_label_sha256(master, "s ap traffic",
                                       transcript_hash_server_finished, 32u,
                                       server_app_secret, 32u) != 0){
        goto done;
    }
    rc = 0;

done:
    crypto_memzero(zero, sizeof(zero));
    crypto_memzero(empty_hash, sizeof(empty_hash));
    crypto_memzero(early, sizeof(early));
    crypto_memzero(d1, sizeof(d1));
    crypto_memzero(hs_secret, sizeof(hs_secret));
    crypto_memzero(d2, sizeof(d2));
    crypto_memzero(master, sizeof(master));
    return rc;
}

static int tls13_build_empty_client_certificate(unsigned char* out, unsigned int out_cap, unsigned int* out_len){
    if (!out || !out_len || out_cap < 8u){
        return -1;
    }
    out[0] = 11u; // certificate
    out[1] = 0u;
    out[2] = 0u;
    out[3] = 4u; // body len
    out[4] = 0u; // certificate_request_context len
    out[5] = 0u; // certificate_list len (u24)
    out[6] = 0u;
    out[7] = 0u;
    *out_len = 8u;
    return 0;
}

static int tls13_build_finished_message(const unsigned char base_secret[32],
                                        const unsigned char transcript_hash[32],
                                        unsigned char* out_msg,
                                        unsigned int out_cap,
                                        unsigned int* out_len){
    if (!base_secret || !transcript_hash || !out_msg || !out_len || out_cap < 36u){
        return -1;
    }
    unsigned char finished_key[32];
    unsigned char verify_data[32];
    if (tls13_hkdf_expand_label_sha256(base_secret, "finished", 0, 0, finished_key, sizeof(finished_key)) != 0){
        return -1;
    }
    crypto_hmac_sha256(finished_key, sizeof(finished_key),
                       transcript_hash, 32u, verify_data);

    out_msg[0] = 20u; // finished
    out_msg[1] = 0u;
    out_msg[2] = 0u;
    out_msg[3] = 32u;
    for (unsigned int i = 0; i < 32u; i++){
        out_msg[4u + i] = verify_data[i];
    }
    *out_len = 36u;
    crypto_memzero(finished_key, sizeof(finished_key));
    crypto_memzero(verify_data, sizeof(verify_data));
    return 0;
}

static int tls13_sigalg_is_supported(unsigned short alg){
    switch (alg){
        case 0x0804u: // rsa_pss_rsae_sha256
        case 0x0805u: // rsa_pss_rsae_sha384
        case 0x0806u: // rsa_pss_rsae_sha512
        case 0x0809u: // rsa_pss_pss_sha256
        case 0x080Au: // rsa_pss_pss_sha384
        case 0x080Bu: // rsa_pss_pss_sha512
        case 0x0403u: // ecdsa_secp256r1_sha256
        case 0x0503u: // ecdsa_secp384r1_sha384
        case 0x0603u: // ecdsa_secp521r1_sha512
        case 0x0807u: // ed25519
        case 0x0401u: // rsa_pkcs1_sha256
        case 0x0501u: // rsa_pkcs1_sha384
        case 0x0601u: // rsa_pkcs1_sha512
            return 1;
        case TLS13_SIGALG_MLDSA65:
            return g_tls13_advertise_pq ? 1 : 0;
        default:
            return 0;
    }
}

static int tls13_verify_server_certificate_verify(const x509_verify_result_t* cert,
                                                  unsigned short sig_alg,
                                                  const unsigned char transcript_hash[32],
                                                  const unsigned char* signature,
                                                  unsigned int signature_len){
    static const char context[] = "TLS 1.3, server CertificateVerify";
    unsigned char signed_msg[64u + sizeof(context) + 32u];
    unsigned int o = 0u;

    if (!cert || !transcript_hash || !signature || signature_len == 0u){
        return -1;
    }
    if (!tls13_sigalg_is_supported(sig_alg)){
        return -1;
    }
    if (cert->leaf_key_alg != X509_VERIFY_KEY_RSA ||
        cert->leaf_rsa_n_len == 0u || cert->leaf_rsa_e_len == 0u){
        return -1;
    }

    for (unsigned int i = 0; i < 64u; i++){
        signed_msg[o++] = 0x20u;
    }
    for (unsigned int i = 0; i < (unsigned int)(sizeof(context) - 1u); i++){
        signed_msg[o++] = (unsigned char)context[i];
    }
    signed_msg[o++] = 0u;
    for (unsigned int i = 0; i < 32u; i++){
        signed_msg[o++] = transcript_hash[i];
    }

    return rsa_verify_x509_signature(cert->leaf_rsa_n, cert->leaf_rsa_n_len,
                                     cert->leaf_rsa_e, cert->leaf_rsa_e_len,
                                     sig_alg,
                                     signed_msg, o,
                                     signature, signature_len);
}

int tcp_https_get(const unsigned char dst_ip[4],
                  const char* host,
                  const char* path,
                  unsigned char* out,
                  unsigned int out_cap){
    const char* req_path = (path && *path) ? path : "/";
    char req[1024];
    int rq = 0;
    unsigned int consumed = 0;
    unsigned int hs_used = 0;
    int saw_server_hello = 0;
    int saw_server_finished = 0;
    int saw_server_certificate = 0;
    int saw_server_certificate_verify = 0;
    int x509_cert_checked = 0;
    unsigned short server_cert_verify_alg = 0u;
    x509_verify_result_t x509_res;
    int need_client_empty_cert = 0;
    int app_bytes = 0;
    int result = -1;
    int fail_code = -1;
    unsigned int rec_len = 0;
    unsigned char rec_type = 0;
    unsigned char rec_hdr[5];
    const unsigned char* rec_payload = 0;
    unsigned int ch_len = 0;
    unsigned int sh_len = 0;
    unsigned char client_priv[32];
    unsigned char client_pub[32];
    unsigned char server_pub[32];
    unsigned char shared[32];
    unsigned char thash[32];
    unsigned char server_finished_expected[32];
    unsigned char server_finished_key[32];
    unsigned char client_hs_finished_msg[64];
    unsigned int client_hs_finished_len = 0;
    unsigned char client_cert_msg[16];
    unsigned int client_cert_len = 0;
    unsigned char ch_msg[1024];
    unsigned char sh_msg[512];
    unsigned char hs_record[1024];
    unsigned int hs_record_len = 0;
    unsigned int app_req_record_len = 0;
    unsigned char transcript_server_finished[32];
    tls13_hs_secrets_t hs_sec;
    tls13_record_ctx_t hs_tx;
    tls13_record_ctx_t hs_rx;
    tls13_record_ctx_t app_tx;
    tls13_record_ctx_t app_rx;
    sha256_ctx_t transcript;

#define HTTPS_FAIL(_code) do { fail_code = (_code); goto https_fail_secure; } while (0)

    if (!dst_ip || !host || !*host || !out || out_cap < 2u){
        g_tcp_stats.http_fail++;
        g_tcp_https_last_error = -100;
        g_tcp_https_fail_count++;
        return g_tcp_https_last_error;
    }
    if (!net_ready() || !net_link_up()){
        g_tcp_stats.http_fail++;
        g_tcp_https_last_error = -101;
        g_tcp_https_fail_count++;
        return g_tcp_https_last_error;
    }

    if (g_conn.active){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
    }

    for (unsigned int i = 0; i < 4; i++){
        g_conn.peer_ip[i] = dst_ip[i];
    }
    g_conn.peer_port = 443u;
    g_conn.local_port = g_next_local_port++;
    if (g_next_local_port < 42000u || g_next_local_port > 52000u){
        g_next_local_port = 42000u;
    }
    g_conn.iss = (unsigned int)(system_ticks * 1664525u + 1013904223u);
    g_conn.snd_una = g_conn.iss;
    g_conn.snd_nxt = g_conn.iss;
    g_conn.rcv_nxt = 0u;
    g_conn.out = g_tls_rx_raw;
    g_conn.out_cap = TCP_TLS_RX_CAP;
    g_conn.out_len = 0u;
    g_conn.last_rx_tick = system_ticks;
    x509_res.chain_certs = 0u;
    x509_res.anchor_count = 0u;
    x509_res.leaf_cert_sig_alg = 0u;
    x509_res.leaf_key_alg = X509_VERIFY_KEY_NONE;
    x509_res.leaf_rsa_n_len = 0u;
    x509_res.leaf_rsa_e_len = 0u;
    x509_res.hostname_ok = 0;
    x509_res.chain_anchor_ok = 0;
    g_conn.state = TCP_ST_SYN_SENT;
    g_conn.active = 1;

    if (tcp_send_segment(TCP_FLAG_SYN, 0, 0) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        g_tcp_https_last_error = -102;
        g_tcp_https_fail_count++;
        return g_tcp_https_last_error;
    }
    g_tcp_stats.syn_sent++;

    if (tls13_wait_for_established() != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        g_tcp_https_last_error = -103;
        g_tcp_https_fail_count++;
        return g_tcp_https_last_error;
    }

    if (x25519_generate_keypair(client_priv, client_pub) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        g_tcp_https_last_error = -104;
        g_tcp_https_fail_count++;
        return g_tcp_https_last_error;
    }

    if (tls13_build_client_hello_sni_x25519(host, client_pub, ch_msg, sizeof(ch_msg), &ch_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-105);
    }

    if (tls13_build_plain_record(22u, ch_msg, ch_len, hs_record, sizeof(hs_record), &hs_record_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-106);
    }
    if (tcp_send_segment((unsigned char)(TCP_FLAG_ACK | TCP_FLAG_PSH), hs_record, hs_record_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-107);
    }

    sha256_init(&transcript);
    sha256_update(&transcript, ch_msg, ch_len);
    hs_used = 0u;
    consumed = 0u;

    while (!saw_server_hello){
        if (tls13_pull_record(&consumed, &rec_type, &rec_payload, &rec_len, rec_hdr, 4500u) != 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-108);
        }
        if (rec_type == 20u){
            continue; // compatibility CCS
        }
        if (rec_type == 21u){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            if (rec_len >= 2u){
                unsigned int alert_desc = rec_payload[1];
                uart_puts("HTTPS TLS alert desc=");
                uart_putdec((unsigned long)alert_desc);
                if (alert_desc == 40u){
                    uart_puts(" (handshake_failure: likely no mutually accepted cert/signature path)\n");
                } else{
                    uart_puts("\n");
                }
                HTTPS_FAIL(-200 - (int)alert_desc);
            }
            HTTPS_FAIL(-109);
        }
        if (rec_type == 23u){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-109);
        }
        if (rec_type != 22u){
            continue;
        }
        if ((hs_used + rec_len) > sizeof(g_tls_hs_buf)){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-110);
        }
        for (unsigned int i = 0; i < rec_len; i++){
            g_tls_hs_buf[hs_used + i] = rec_payload[i];
        }
        hs_used += rec_len;

        if (hs_used < 4u){
            continue;
        }
        unsigned int mlen = be24_read(&g_tls_hs_buf[1]);
        unsigned int mtotal = 4u + mlen;
        if (mtotal > hs_used || mtotal > sizeof(sh_msg)){
            continue;
        }
        if (g_tls_hs_buf[0] == 2u){
            for (unsigned int i = 0; i < mtotal; i++){
                sh_msg[i] = g_tls_hs_buf[i];
            }
            sh_len = mtotal;
            saw_server_hello = 1;
            sha256_update(&transcript, sh_msg, sh_len);
            unsigned int rem = hs_used - mtotal;
            for (unsigned int i = 0; i < rem; i++){
                g_tls_hs_buf[i] = g_tls_hs_buf[mtotal + i];
            }
            hs_used = rem;
        } else{
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-111);
        }
    }

    if (tls13_process_server_hello_x25519(sh_msg, sh_len, server_pub) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-112);
    }
    if (x25519_shared_secret(client_priv, server_pub, shared) != 0 || x25519_is_all_zero(shared)){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-113);
    }
    sha256_snapshot(&transcript, thash);
    if (tls13_derive_handshake_secrets_sha256(shared, thash, &hs_sec) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-114);
    }
    if (tls13_record_init(&hs_tx, hs_sec.client_key, TLS13_KEY_BYTES, hs_sec.client_iv) != 0 ||
        tls13_record_init(&hs_rx, hs_sec.server_key, TLS13_KEY_BYTES, hs_sec.server_iv) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-115);
    }

    while (!saw_server_finished){
        if (tls13_pull_record(&consumed, &rec_type, &rec_payload, &rec_len, rec_hdr, 5500u) != 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-116);
        }

        if (rec_type == 20u){
            continue;
        }
        if (rec_type == 21u){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-117);
        }
        if (rec_type != 23u){
            continue;
        }
        if (rec_len > TCP_TLS_REC_MAX){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-118);
        }

        for (unsigned int h = 0; h < 5u; h++){
            g_tls_record_wire[h] = rec_hdr[h];
        }
        for (unsigned int i = 0; i < rec_len; i++){
            g_tls_record_wire[5u + i] = rec_payload[i];
        }
        unsigned int plain_len = 0;
        unsigned char inner_type = 0;
        if (tls13_record_decrypt(&hs_rx,
                                 g_tls_record_wire, 5u + rec_len,
                                 g_tls_record_plain, sizeof(g_tls_record_plain),
                                 &plain_len, &inner_type) != 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-119);
        }
        if (inner_type != 22u || plain_len == 0u){
            continue;
        }
        if ((hs_used + plain_len) > sizeof(g_tls_hs_buf)){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-120);
        }
        for (unsigned int i = 0; i < plain_len; i++){
            g_tls_hs_buf[hs_used + i] = g_tls_record_plain[i];
        }
        hs_used += plain_len;

        unsigned int parsed = 0;
        while ((hs_used - parsed) >= 4u){
            unsigned int msg_len = be24_read(&g_tls_hs_buf[parsed + 1u]);
            unsigned int msg_tot = 4u + msg_len;
            if ((hs_used - parsed) < msg_tot){
                break;
            }
            unsigned char hs_type = g_tls_hs_buf[parsed];
            const unsigned char* hs_ptr = &g_tls_hs_buf[parsed];

            if (hs_type == 11u){
                // TLS 1.3 Certificate + X.509 hostname/anchor validation
                if (msg_tot <= 4u){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-140);
                }
                if (x509_verify_tls13_certificate(host,
                                                  &hs_ptr[4],
                                                  msg_tot - 4u,
                                                  server_cert_verify_alg,
                                                  &x509_res) != 0){
                    uart_puts("HTTPS X509 verify failed for host ");
                    uart_puts(host);
                    uart_puts("\n");
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-141);
                }
                saw_server_certificate = 1;
                x509_cert_checked = 1;
            }

            if (hs_type == 15u){
                // TLS 1.3 CertificateVerify: SignatureScheme(2) + sig_len(2) + signature
                if (msg_tot < 8u){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-137);
                }
                unsigned short sig_alg = be16_read(&hs_ptr[4]);
                unsigned short sig_len = be16_read(&hs_ptr[6]);
                unsigned int body_len = msg_tot - 4u;
                if ((unsigned int)sig_len != (body_len - 4u) || !tls13_sigalg_is_supported(sig_alg)){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-138);
                }
                if (!x509_cert_checked){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-139);
                }
                sha256_snapshot(&transcript, thash);
                if (tls13_verify_server_certificate_verify(&x509_res,
                                                           sig_alg,
                                                           thash,
                                                           &hs_ptr[8],
                                                           (unsigned int)sig_len) != 0){
                    uart_puts("HTTPS CertificateVerify failed alg=");
                    uart_puthex((unsigned int)sig_alg);
                    uart_puts("\n");
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-142);
                }
                saw_server_certificate_verify = 1;
                server_cert_verify_alg = sig_alg;
            }

            if (hs_type == 13u){
                need_client_empty_cert = 1;
            }

            if (hs_type == 20u){
                if (!saw_server_certificate || !saw_server_certificate_verify || !x509_cert_checked){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-139);
                }
                if (msg_tot != 36u){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-121);
                }
                if (tls13_hkdf_expand_label_sha256(hs_sec.server_hs_traffic_secret, "finished",
                                                   0, 0, server_finished_key, sizeof(server_finished_key)) != 0){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-122);
                }
                sha256_snapshot(&transcript, thash);
                crypto_hmac_sha256(server_finished_key, sizeof(server_finished_key), thash, sizeof(thash), server_finished_expected);
                if (!crypto_consttime_equal(server_finished_expected, &hs_ptr[4], 32u)){
                    g_conn.active = 0;
                    g_conn.state = TCP_ST_CLOSED;
                    g_tcp_stats.http_fail++;
                    HTTPS_FAIL(-123);
                }
                sha256_update(&transcript, hs_ptr, msg_tot);
                saw_server_finished = 1;
                sha256_snapshot(&transcript, transcript_server_finished);
                parsed += msg_tot;
                break;
            }

            sha256_update(&transcript, hs_ptr, msg_tot);
            parsed += msg_tot;
        }

        if (parsed > 0u){
            unsigned int rem = hs_used - parsed;
            for (unsigned int i = 0; i < rem; i++){
                g_tls_hs_buf[i] = g_tls_hs_buf[parsed + i];
            }
            hs_used = rem;
        }
    }

    if (need_client_empty_cert){
        if (tls13_build_empty_client_certificate(client_cert_msg, sizeof(client_cert_msg), &client_cert_len) != 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-124);
        }
        if (tls13_record_encrypt(&hs_tx, 22u, client_cert_msg, client_cert_len,
                                 g_tls_record_tx, sizeof(g_tls_record_tx), &hs_record_len) != 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-125);
        }
        if (tcp_send_segment((unsigned char)(TCP_FLAG_ACK | TCP_FLAG_PSH), g_tls_record_tx, hs_record_len) != 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            HTTPS_FAIL(-126);
        }
        sha256_update(&transcript, client_cert_msg, client_cert_len);
    }

    sha256_snapshot(&transcript, thash);
    if (tls13_build_finished_message(hs_sec.client_hs_traffic_secret, thash,
                                     client_hs_finished_msg, sizeof(client_hs_finished_msg),
                                     &client_hs_finished_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-127);
    }
    if (tls13_record_encrypt(&hs_tx, 22u,
                             client_hs_finished_msg, client_hs_finished_len,
                             g_tls_record_tx, sizeof(g_tls_record_tx), &hs_record_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-128);
    }
    if (tcp_send_segment((unsigned char)(TCP_FLAG_ACK | TCP_FLAG_PSH), g_tls_record_tx, hs_record_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-129);
    }
    sha256_update(&transcript, client_hs_finished_msg, client_hs_finished_len);

    unsigned char c_app_secret[32];
    unsigned char s_app_secret[32];
    unsigned char c_app_key[16];
    unsigned char s_app_key[16];
    unsigned char c_app_iv[12];
    unsigned char s_app_iv[12];

    if (tls13_derive_master_and_app_secrets(shared,
                                            transcript_server_finished,
                                            c_app_secret, s_app_secret) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-130);
    }
    if (tls13_hkdf_expand_label_sha256(c_app_secret, "key", 0, 0, c_app_key, sizeof(c_app_key)) != 0 ||
        tls13_hkdf_expand_label_sha256(s_app_secret, "key", 0, 0, s_app_key, sizeof(s_app_key)) != 0 ||
        tls13_hkdf_expand_label_sha256(c_app_secret, "iv", 0, 0, c_app_iv, sizeof(c_app_iv)) != 0 ||
        tls13_hkdf_expand_label_sha256(s_app_secret, "iv", 0, 0, s_app_iv, sizeof(s_app_iv)) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-131);
    }
    if (tls13_record_init(&app_tx, c_app_key, sizeof(c_app_key), c_app_iv) != 0 ||
        tls13_record_init(&app_rx, s_app_key, sizeof(s_app_key), s_app_iv) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-132);
    }

    if (append_str(req, (int)sizeof(req), &rq, "GET ") != 0 ||
        append_str(req, (int)sizeof(req), &rq, req_path) != 0 ||
        append_str(req, (int)sizeof(req), &rq, " HTTP/1.1\r\nHost: ") != 0 ||
        append_str(req, (int)sizeof(req), &rq, host) != 0 ||
        append_str(req, (int)sizeof(req), &rq,
                   "\r\nUser-Agent: Mozilla/5.0 (compatible; QOS/0.1)\r\n"
                   "Accept: text/html,text/plain,*/*;q=0.8\r\n"
                   "Accept-Encoding: identity\r\n"
                   "Connection: close\r\n\r\n") != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-133);
    }
    if (tls13_record_encrypt(&app_tx, 23u, (const unsigned char*)req, (unsigned int)rq,
                             g_tls_record_tx, sizeof(g_tls_record_tx), &app_req_record_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-134);
    }
    if (tcp_send_segment((unsigned char)(TCP_FLAG_ACK | TCP_FLAG_PSH),
                         g_tls_record_tx, app_req_record_len) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        HTTPS_FAIL(-135);
    }

    out[0] = 0;
    app_bytes = 0;
    hs_used = 0u;
    unsigned long start_tick = system_ticks;
    unsigned long last_progress_tick = system_ticks;

    while (1){
        int completion = (app_bytes > 0) ? http_response_completion_state(out, (unsigned int)app_bytes) : 0;
        if (completion == 1){
            break;
        }
        unsigned int pull_timeout = (app_bytes > 0 && completion < 0) ? 350u : 2500u;
        if (tls13_pull_record(&consumed, &rec_type, &rec_payload, &rec_len, rec_hdr, pull_timeout) != 0){
            break;
        }
        if (rec_type == 21u){
            break;
        }
        if (rec_type != 23u){
            continue;
        }
        if (rec_len > TCP_TLS_REC_MAX){
            break;
        }
        for (unsigned int h = 0; h < 5u; h++){
            g_tls_record_wire[h] = rec_hdr[h];
        }
        for (unsigned int i = 0; i < rec_len; i++){
            g_tls_record_wire[5u + i] = rec_payload[i];
        }

        unsigned int p_len = 0;
        unsigned char inner = 0;
        if (tls13_record_decrypt(&app_rx,
                                 g_tls_record_wire, 5u + rec_len,
                                 g_tls_record_plain, sizeof(g_tls_record_plain),
                                 &p_len, &inner) != 0){
            break;
        }

        if (inner == 23u && p_len > 0u){
            unsigned int room = (app_bytes < (int)(out_cap - 1u)) ? ((out_cap - 1u) - (unsigned int)app_bytes) : 0u;
            unsigned int take = (p_len < room) ? p_len : room;
            for (unsigned int i = 0; i < take; i++){
                out[app_bytes + (int)i] = g_tls_record_plain[i];
            }
            app_bytes += (int)take;
            out[app_bytes] = 0;
            last_progress_tick = system_ticks;
            if (http_response_completion_state(out, (unsigned int)app_bytes) == 1){
                break;
            }
            if (room == 0u){
                break;
            }
        } else if (inner == 22u && p_len > 0u){
            // Post-handshake messages (e.g., NewSessionTicket). Ignore for now.
        }

        if (g_conn.state == TCP_ST_CLOSE_WAIT){
            if ((unsigned long)(system_ticks - last_progress_tick) > 250u){
                break;
            }
        } else{
            if ((unsigned long)(system_ticks - start_tick) > 9000u){
                break;
            }
            if ((unsigned long)(system_ticks - last_progress_tick) > 2000u && app_bytes > 0){
                break;
            }
        }
    }

    g_conn.active = 0;
    g_conn.state = TCP_ST_CLOSED;
    out[(app_bytes >= 0 && (unsigned int)app_bytes < out_cap) ? (unsigned int)app_bytes : (out_cap - 1u)] = 0;
    if (app_bytes > 0){
        g_tcp_stats.http_ok++;
        result = app_bytes;
        g_tcp_https_last_error = 0;
    } else{
        g_tcp_stats.http_fail++;
        result = -1;
        fail_code = -136;
    }

https_fail_secure:
    crypto_memzero(client_priv, sizeof(client_priv));
    crypto_memzero(client_pub, sizeof(client_pub));
    crypto_memzero(server_pub, sizeof(server_pub));
    crypto_memzero(shared, sizeof(shared));
    crypto_memzero(thash, sizeof(thash));
    crypto_memzero(server_finished_expected, sizeof(server_finished_expected));
    crypto_memzero(server_finished_key, sizeof(server_finished_key));
    crypto_memzero(client_hs_finished_msg, sizeof(client_hs_finished_msg));
    crypto_memzero(client_cert_msg, sizeof(client_cert_msg));
    crypto_memzero(ch_msg, sizeof(ch_msg));
    crypto_memzero(sh_msg, sizeof(sh_msg));
    crypto_memzero(hs_record, sizeof(hs_record));
    crypto_memzero(transcript_server_finished, sizeof(transcript_server_finished));
    crypto_memzero(&hs_sec, sizeof(hs_sec));
    crypto_memzero(&hs_tx, sizeof(hs_tx));
    crypto_memzero(&hs_rx, sizeof(hs_rx));
    crypto_memzero(&app_tx, sizeof(app_tx));
    crypto_memzero(&app_rx, sizeof(app_rx));
    crypto_memzero(g_tls_hs_buf, sizeof(g_tls_hs_buf));
    crypto_memzero(g_tls_record_wire, sizeof(g_tls_record_wire));
    crypto_memzero(g_tls_record_plain, sizeof(g_tls_record_plain));
    crypto_memzero(g_tls_record_tx, sizeof(g_tls_record_tx));

    if (result < 0){
        g_tcp_https_last_error = fail_code;
        g_tcp_https_fail_count++;
    } else if (x509_res.hostname_ok && x509_res.chain_anchor_ok){
        uart_puts("HTTPS X509: host+anchor OK chain=");
        uart_putdec((unsigned long)x509_res.chain_certs);
        uart_puts(" anchors=");
        uart_putdec((unsigned long)x509_res.anchor_count);
        uart_puts(" leaf_sig_alg=");
        uart_puthex((unsigned int)x509_res.leaf_cert_sig_alg);
        uart_puts(" cert_verify_alg=");
        uart_puthex((unsigned int)server_cert_verify_alg);
        uart_puts("\n");
    } else if (server_cert_verify_alg != 0u){
        uart_puts("HTTPS cert signature alg=");
        uart_puthex((unsigned int)server_cert_verify_alg);
        uart_puts("\n");
    }

#undef HTTPS_FAIL

    return (result < 0) ? fail_code : result;
}

int tcp_http_get(const unsigned char dst_ip[4],
                 const char* host,
                 const char* path,
                 unsigned char* out,
                 unsigned int out_cap){
    char req[1024];
    int rq = 0;
    unsigned long start_tick;
    unsigned long last_progress;
    unsigned long freq;
    unsigned long start_cnt;
    unsigned long last_progress_cnt;
    unsigned long handshake_to_cnt;
    unsigned long overall_to_cnt;
    unsigned long idle_to_cnt;
    unsigned long spin_budget;
    const char* req_path = path && *path ? path : "/";

    if (!dst_ip || !host || !*host || !out || out_cap == 0){
        g_tcp_stats.http_fail++;
        return -1;
    }
    if (!net_ready() || !net_link_up()){
        g_tcp_stats.http_fail++;
        return -1;
    }

    if (g_conn.active){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
    }

    for (unsigned int i = 0; i < 4; i++){
        g_conn.peer_ip[i] = dst_ip[i];
    }
    g_conn.peer_port = 80u;
    g_conn.local_port = g_next_local_port++;
    if (g_next_local_port < 42000u || g_next_local_port > 52000u){
        g_next_local_port = 42000u;
    }
    g_conn.iss = (unsigned int)(system_ticks * 1103515245u + 12345u);
    g_conn.snd_una = g_conn.iss;
    g_conn.snd_nxt = g_conn.iss;
    g_conn.rcv_nxt = 0u;
    g_conn.out = out;
    g_conn.out_cap = out_cap - 1u; // reserve null terminator slot
    g_conn.out_len = 0u;
    g_conn.last_rx_tick = system_ticks;
    g_conn.state = TCP_ST_SYN_SENT;
    g_conn.active = 1;

    if (tcp_send_segment(TCP_FLAG_SYN, 0, 0) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }
    g_tcp_stats.syn_sent++;

    freq = read_cntfrq();
    if (freq == 0){
        freq = 1000000UL;
    }
    handshake_to_cnt = (freq / 1000UL) * 2500UL; // 2.5s
    overall_to_cnt = (freq / 1000UL) * 9000UL;   // 9s
    idle_to_cnt = (freq / 1000UL) * 1500UL;      // 1.5s
    if (handshake_to_cnt == 0) handshake_to_cnt = freq;
    if (overall_to_cnt == 0) overall_to_cnt = freq * 2UL;
    if (idle_to_cnt == 0) idle_to_cnt = freq / 2UL;

    start_tick = system_ticks;
    start_cnt = read_cntpct();
    spin_budget = 25000000UL;
    while (g_conn.state == TCP_ST_SYN_SENT){
        (void)net_poll();
        if ((read_cntpct() - start_cnt) > handshake_to_cnt){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            return -1;
        }
        if ((long)(system_ticks - start_tick) > 2000){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            return -1;
        }
        if (spin_budget-- == 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            return -1;
        }
    }
    if (g_conn.state != TCP_ST_ESTABLISHED){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }

    if (append_str(req, (int)sizeof(req), &rq, "GET ") != 0 ||
        append_str(req, (int)sizeof(req), &rq, req_path) != 0 ||
        append_str(req, (int)sizeof(req), &rq, " HTTP/1.1\r\nHost: ") != 0 ||
        append_str(req, (int)sizeof(req), &rq, host) != 0 ||
        append_str(req, (int)sizeof(req), &rq,
                   "\r\nUser-Agent: Mozilla/5.0 (compatible; QOS/0.1)\r\n"
                   "Accept: text/html,text/plain,*/*;q=0.8\r\n"
                   "Accept-Encoding: identity\r\n"
                   "Connection: close\r\n\r\n") != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }

    if (tcp_send_segment((unsigned char)(TCP_FLAG_ACK | TCP_FLAG_PSH), (const unsigned char*)req, (unsigned int)rq) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }

    start_tick = system_ticks;
    last_progress = system_ticks;
    start_cnt = read_cntpct();
    last_progress_cnt = start_cnt;
    unsigned int prev_len = 0u;
    spin_budget = 80000000UL;
    while (1){
        (void)net_poll();
        if (g_conn.out_len != prev_len){
            prev_len = g_conn.out_len;
            last_progress = system_ticks;
            last_progress_cnt = read_cntpct();
        }
        if (g_conn.state == TCP_ST_CLOSE_WAIT){
            break;
        }
        if (g_conn.out_len >= g_conn.out_cap){
            break;
        }
        if ((read_cntpct() - start_cnt) > overall_to_cnt){
            break;
        }
        if ((read_cntpct() - last_progress_cnt) > idle_to_cnt && g_conn.out_len > 0){
            break;
        }
        if ((long)(system_ticks - last_progress) > 1200 && g_conn.out_len > 0){
            break;
        }
        if ((long)(system_ticks - start_tick) > 7000){
            break;
        }
        if (spin_budget-- == 0){
            break;
        }
    }

    g_conn.active = 0;
    g_conn.state = TCP_ST_CLOSED;
    out[g_conn.out_len] = 0;
    if (g_conn.out_len > 0u){
        g_tcp_stats.http_ok++;
        return (int)g_conn.out_len;
    }
    g_tcp_stats.http_fail++;
    return -1;
}

void tcp_dump_stats(void){
    uart_puts("TCP rx=");
    uart_putdec(g_tcp_stats.rx_total);
    uart_puts(" match=");
    uart_putdec(g_tcp_stats.rx_match);
    uart_puts(" rx_data=");
    uart_putdec(g_tcp_stats.rx_data);
    uart_puts(" rx_drop=");
    uart_putdec(g_tcp_stats.rx_drop);
    uart_puts(" tx=");
    uart_putdec(g_tcp_stats.tx_total);
    uart_puts(" tx_fail=");
    uart_putdec(g_tcp_stats.tx_fail);
    uart_puts(" syn=");
    uart_putdec(g_tcp_stats.syn_sent);
    uart_puts(" synack=");
    uart_putdec(g_tcp_stats.synack_rx);
    uart_puts(" est=");
    uart_putdec(g_tcp_stats.est_ok);
    uart_puts(" fin=");
    uart_putdec(g_tcp_stats.fin_rx);
    uart_puts(" http_ok=");
    uart_putdec(g_tcp_stats.http_ok);
    uart_puts(" http_fail=");
    uart_putdec(g_tcp_stats.http_fail);
    uart_puts(" https_last_err=");
    if (g_tcp_https_last_error < 0){
        uart_puts("-");
        uart_putdec((unsigned long)(-g_tcp_https_last_error));
    } else{
        uart_putdec((unsigned long)g_tcp_https_last_error);
    }
    uart_puts(" https_fail_ct=");
    uart_putdec(g_tcp_https_fail_count);
    uart_puts("\n");
}
