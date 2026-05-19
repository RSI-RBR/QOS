#include "remote_login.h"
#include "auth.h"
#include "udp.h"
#include "x25519.h"
#include "pq_kem.h"
#include "crypto.h"
#include "aes_gcm.h"
#include "uart.h"
#include "klog.h"
#include "timer.h"
#include "panic.h"
#include "headless_control.h"
#include "terminal.h"
#include "net.h"
#include "arp.h"
#include "platform/board_config.h"

#define uart_puts klog_puts
#define uart_putdec klog_putdec

#define RLOGIN_PORT 2222u
#define RLOGIN_MAGIC 0x51524C47u /* QRLG */
#define RLOGIN_VER 1u

#define RLOGIN_TYPE_CLIENT_HELLO 1u
#define RLOGIN_TYPE_SERVER_HELLO 2u
#define RLOGIN_TYPE_AUTH_PROOF 3u
#define RLOGIN_TYPE_AUTH_RESULT 4u
#define RLOGIN_TYPE_COMMAND 5u
#define RLOGIN_TYPE_COMMAND_RESULT 6u
#define RLOGIN_TYPE_TTY_INPUT 7u
#define RLOGIN_TYPE_TTY_OUTPUT 8u

#define RLOGIN_PAYLOAD_MAX 1400u
#define RLOGIN_TAG_LEN 16u
#define RLOGIN_SERVER_HELLO_BASE_LEN (32u + AUTH_NONCE_BYTES + AUTH_SALT_BYTES + 1u)
#define RLOGIN_SERVER_HELLO_KDF_EXT_LEN (1u + 4u + 4u + 4u + 4u)
#define RLOGIN_KEX_EXT_FIXED_LEN 8u
#define RLOGIN_SERVER_HELLO_MAX_LEN (RLOGIN_SERVER_HELLO_BASE_LEN + RLOGIN_SERVER_HELLO_KDF_EXT_LEN + RLOGIN_KEX_EXT_FIXED_LEN + QOS_KEM_MLKEM768_CT_BYTES)
#define RLOGIN_TTY_IN_CAP 512u
#define RLOGIN_TTY_OUT_CAP 2048u
#define RLOGIN_TTY_OUT_CHUNK 220u
#define RLOGIN_REPLAY_WINDOW 32u

#define RLOGIN_KEX_EXT_MAGIC 0x524B4558u /* RKEX */
#define RLOGIN_KEX_EXT_VER 1u
#define RLOGIN_KEX_X25519 1u
#define RLOGIN_KEX_MLKEM768 2u
#define RLOGIN_KEX_HYBRID 3u

#define RLOGIN_STATUS_OK 0u
#define RLOGIN_STATUS_AUTH_UNAVAILABLE 1u
#define RLOGIN_STATUS_RATE_LIMITED 2u
#define RLOGIN_STATUS_LOCKED 3u

#define RLOGIN_AUTH_BACKOFF_MS 5000u
#define RLOGIN_AUTH_LOCKOUT_FAIL_THRESHOLD 5u
#define RLOGIN_AUTH_LOCKOUT_MS 60000u

typedef struct {
    unsigned long rx_total;
    unsigned long tx_total;
    unsigned long bad_header;
    unsigned long bad_crypto;
    unsigned long auth_ok;
    unsigned long auth_fail;
    unsigned long auth_rate_limited;
    unsigned long auth_locked;
    unsigned long replay_drop;
    unsigned long cmd_ok;
    unsigned long cmd_fail;
    unsigned long tty_in_bytes;
    unsigned long tty_out_bytes;
    unsigned long kex_x25519;
    unsigned long kex_mlkem768;
    unsigned long kex_hybrid;
} remote_login_stats_t;

typedef struct {
    int active;
    int authed;
    int tty_attached;
    unsigned char src_ip[4];
    unsigned short src_port;
    unsigned int session_id;
    unsigned int client_seq_top;
    unsigned int client_seq_seen_bitmap;
    unsigned int server_next_seq;
    unsigned char kex_mode;
    char username[AUTH_USERNAME_MAX + 1u];
    unsigned char client_pub[32];
    unsigned char server_priv[32];
    unsigned char server_pub[32];
    unsigned char client_mlkem_pk[QOS_KEM_MLKEM768_PK_BYTES];
    unsigned char server_mlkem_ct[QOS_KEM_MLKEM768_CT_BYTES];
    unsigned char client_nonce[AUTH_NONCE_BYTES];
    unsigned char server_nonce[AUTH_NONCE_BYTES];
    unsigned char key[32];
    unsigned char nonce_base[12];
} remote_login_session_t;

static remote_login_stats_t g_stats;
static remote_login_session_t g_sess;
static int g_enabled = 0;
static int g_auth_ready = 0;
static unsigned char g_tty_in_q[RLOGIN_TTY_IN_CAP];
static unsigned int g_tty_in_head = 0;
static unsigned int g_tty_in_tail = 0;
static unsigned int g_tty_in_count = 0;
static unsigned char g_tty_out_q[RLOGIN_TTY_OUT_CAP];
static unsigned int g_tty_out_head = 0;
static unsigned int g_tty_out_tail = 0;
static unsigned int g_tty_out_count = 0;
static unsigned int g_auth_fail_streak = 0;
static unsigned long g_auth_delay_until_tick = 0;
static unsigned long g_auth_lockout_until_tick = 0;

static void rlogin_headless_write(const char* s){
    unsigned long len = 0;
    if (!s){
        return;
    }
    while (s[len]){
        len++;
    }
    terminal_write(0, -1, s, len);
}

static void rlogin_headless_putdec(unsigned long v){
    char tmp[21];
    char out[21];
    int n = 0;
    int o = 0;

    if (v == 0UL){
        rlogin_headless_write("0");
        return;
    }
    while (v > 0UL && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0 && o + 1 < (int)sizeof(out)){
        out[o++] = tmp[--n];
    }
    out[o] = 0;
    rlogin_headless_write(out);
}

static void rlogin_headless_diag_tick(void){
#if defined(QOS_BOARD_PI_ZERO2W) && QOS_BOARD_PI_ZERO2W
    static unsigned long next_diag_tick = 0;
    unsigned long now = system_ticks;
    unsigned long net_rx = 0;
    unsigned long net_drop = 0;
    unsigned long net_tx = 0;
    unsigned long net_txfail = 0;
    unsigned int net_rxq = 0;
    const char* driver = "none";
    int link = 0;
    unsigned long arp_rx = 0;
    unsigned long arp_tx_req = 0;
    unsigned long arp_tx_rep = 0;
    unsigned int arp_cache = 0;
    int arp_gw = 0;

    if ((long)(now - next_diag_tick) < 0){
        return;
    }
    next_diag_tick = now + 5000u;
    if (!g_enabled || g_sess.authed){
        return;
    }

    net_get_diag(&net_rx, &net_drop, &net_tx, &net_txfail, &net_rxq, &driver, &link);
    arp_get_diag(&arp_rx, &arp_tx_req, &arp_tx_rep, &arp_cache, &arp_gw);

    rlogin_headless_write("Pi0 diag: rstate=");
    rlogin_headless_putdec((unsigned long)remote_login_state_bits());
    rlogin_headless_write(" rrx=");
    rlogin_headless_putdec(g_stats.rx_total);
    rlogin_headless_write(" rtx=");
    rlogin_headless_putdec(g_stats.tx_total);
    rlogin_headless_write(" bh=");
    rlogin_headless_putdec(g_stats.bad_header);
    rlogin_headless_write(" bc=");
    rlogin_headless_putdec(g_stats.bad_crypto);
    rlogin_headless_write("\n");

    rlogin_headless_write("Pi0 net: drv=");
    rlogin_headless_write(driver ? driver : "none");
    rlogin_headless_write(" link=");
    rlogin_headless_putdec((unsigned long)(link ? 1 : 0));
    rlogin_headless_write(" rx=");
    rlogin_headless_putdec(net_rx);
    rlogin_headless_write(" drop=");
    rlogin_headless_putdec(net_drop);
    rlogin_headless_write(" tx=");
    rlogin_headless_putdec(net_tx);
    rlogin_headless_write(" txf=");
    rlogin_headless_putdec(net_txfail);
    rlogin_headless_write(" rxq=");
    rlogin_headless_putdec((unsigned long)net_rxq);
    rlogin_headless_write("\n");

    rlogin_headless_write("Pi0 arp: rx=");
    rlogin_headless_putdec(arp_rx);
    rlogin_headless_write(" txreq=");
    rlogin_headless_putdec(arp_tx_req);
    rlogin_headless_write(" txrep=");
    rlogin_headless_putdec(arp_tx_rep);
    rlogin_headless_write(" cache=");
    rlogin_headless_putdec((unsigned long)arp_cache);
    rlogin_headless_write(" gw=");
    rlogin_headless_putdec((unsigned long)(arp_gw ? 1 : 0));
    rlogin_headless_write("\n");
#endif
}

static unsigned short read_be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static unsigned int read_be32(const unsigned char* p){
    return ((unsigned int)p[0] << 24) |
           ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) |
           ((unsigned int)p[3]);
}

static void write_be16(unsigned char* p, unsigned short v){
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFFu);
}

static void write_be32(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)((v >> 24) & 0xFFu);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8) & 0xFFu);
    p[3] = (unsigned char)(v & 0xFFu);
}

static int ip4_eq(const unsigned char a[4], const unsigned char b[4]){
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int kex_mode_valid(unsigned char kex_mode){
    return (kex_mode == RLOGIN_KEX_X25519 ||
            kex_mode == RLOGIN_KEX_MLKEM768 ||
            kex_mode == RLOGIN_KEX_HYBRID) ? 1 : 0;
}

static const char* kex_mode_name(unsigned char kex_mode){
    if (kex_mode == RLOGIN_KEX_MLKEM768){
        return "ML-KEM-768";
    }
    if (kex_mode == RLOGIN_KEX_HYBRID){
        return "ML-KEM-768+X25519";
    }
    return "X25519";
}

static int tick_before(unsigned long a, unsigned long b){
    return (long)(a - b) < 0;
}

static int tick_window_active(unsigned long now, unsigned long until){
    if (until == 0u){
        return 0;
    }
    return tick_before(now, until);
}

static unsigned char auth_gate_status(void){
    unsigned long now = system_ticks;
    if (tick_window_active(now, g_auth_lockout_until_tick)){
        return RLOGIN_STATUS_LOCKED;
    }
    if (tick_window_active(now, g_auth_delay_until_tick)){
        return RLOGIN_STATUS_RATE_LIMITED;
    }
    return RLOGIN_STATUS_OK;
}

static void auth_mark_failure(void){
    unsigned long now = system_ticks;
    g_auth_delay_until_tick = now + RLOGIN_AUTH_BACKOFF_MS;
    if (g_auth_fail_streak < 0xFFFFFFFFu){
        g_auth_fail_streak++;
    }
    if (g_auth_fail_streak >= RLOGIN_AUTH_LOCKOUT_FAIL_THRESHOLD){
        g_auth_lockout_until_tick = now + RLOGIN_AUTH_LOCKOUT_MS;
    }
}

static void auth_mark_success(void){
    g_auth_fail_streak = 0;
    g_auth_delay_until_tick = 0;
    g_auth_lockout_until_tick = 0;
}

static int client_seq_is_fresh(unsigned int seq){
    if (seq == 0u){
        return 0;
    }
    if (g_sess.client_seq_top == 0u){
        return 1;
    }
    if (seq > g_sess.client_seq_top){
        return 1;
    }
    unsigned int back = g_sess.client_seq_top - seq;
    if (back >= RLOGIN_REPLAY_WINDOW){
        return 0;
    }
    if ((g_sess.client_seq_seen_bitmap & (1u << back)) != 0u){
        return 0;
    }
    return 1;
}

static void client_seq_mark_seen(unsigned int seq){
    if (seq == 0u){
        return;
    }
    if (g_sess.client_seq_top == 0u){
        g_sess.client_seq_top = seq;
        g_sess.client_seq_seen_bitmap = 1u;
        return;
    }
    if (seq > g_sess.client_seq_top){
        unsigned int delta = seq - g_sess.client_seq_top;
        if (delta >= RLOGIN_REPLAY_WINDOW){
            g_sess.client_seq_seen_bitmap = 0u;
        } else{
            g_sess.client_seq_seen_bitmap <<= delta;
        }
        g_sess.client_seq_seen_bitmap |= 1u;
        g_sess.client_seq_top = seq;
        return;
    }
    unsigned int back = g_sess.client_seq_top - seq;
    if (back < RLOGIN_REPLAY_WINDOW){
        g_sess.client_seq_seen_bitmap |= (1u << back);
    }
}

static void tty_queues_clear(void){
    g_tty_in_head = 0;
    g_tty_in_tail = 0;
    g_tty_in_count = 0;
    g_tty_out_head = 0;
    g_tty_out_tail = 0;
    g_tty_out_count = 0;
}

static void session_clear(void){
    g_sess.active = 0;
    g_sess.authed = 0;
    g_sess.tty_attached = 0;
    g_sess.src_ip[0] = 0;
    g_sess.src_ip[1] = 0;
    g_sess.src_ip[2] = 0;
    g_sess.src_ip[3] = 0;
    g_sess.src_port = 0;
    g_sess.session_id = 0;
    g_sess.client_seq_top = 0;
    g_sess.client_seq_seen_bitmap = 0;
    g_sess.server_next_seq = 0;
    g_sess.kex_mode = RLOGIN_KEX_X25519;
    for (unsigned int i = 0; i < sizeof(g_sess.username); i++){
        g_sess.username[i] = 0;
    }
    crypto_memzero(g_sess.client_pub, sizeof(g_sess.client_pub));
    crypto_memzero(g_sess.server_priv, sizeof(g_sess.server_priv));
    crypto_memzero(g_sess.server_pub, sizeof(g_sess.server_pub));
    crypto_memzero(g_sess.client_mlkem_pk, sizeof(g_sess.client_mlkem_pk));
    crypto_memzero(g_sess.server_mlkem_ct, sizeof(g_sess.server_mlkem_ct));
    crypto_memzero(g_sess.client_nonce, sizeof(g_sess.client_nonce));
    crypto_memzero(g_sess.server_nonce, sizeof(g_sess.server_nonce));
    crypto_memzero(g_sess.key, sizeof(g_sess.key));
    crypto_memzero(g_sess.nonce_base, sizeof(g_sess.nonce_base));
    tty_queues_clear();
}

static void session_copy_peer(const udp_meta_t* meta){
    for (unsigned int i = 0; i < 4; i++){
        g_sess.src_ip[i] = meta->src_ip[i];
    }
    g_sess.src_port = meta->src_port;
}

static int build_header(unsigned char* out,
                        unsigned char type,
                        unsigned int session_id,
                        unsigned int seq,
                        unsigned short payload_len){
    if (!out){
        return -1;
    }
    write_be32(&out[0], RLOGIN_MAGIC);
    out[4] = RLOGIN_VER;
    out[5] = type;
    write_be16(&out[6], 0u);
    write_be32(&out[8], session_id);
    write_be32(&out[12], seq);
    write_be16(&out[16], payload_len);
    write_be16(&out[18], 0u);
    return 20;
}

static int parse_header(const unsigned char* in,
                        unsigned int in_len,
                        unsigned char* out_type,
                        unsigned int* out_session_id,
                        unsigned int* out_seq,
                        unsigned short* out_payload_len){
    if (!in || in_len < 20u || !out_type || !out_session_id || !out_seq || !out_payload_len){
        return -1;
    }
    if (read_be32(&in[0]) != RLOGIN_MAGIC || in[4] != RLOGIN_VER){
        return -1;
    }
    *out_type = in[5];
    *out_session_id = read_be32(&in[8]);
    *out_seq = read_be32(&in[12]);
    *out_payload_len = read_be16(&in[16]);
    if (*out_payload_len > RLOGIN_PAYLOAD_MAX){
        return -1;
    }
    if ((unsigned int)(20u + *out_payload_len) > in_len){
        return -1;
    }
    return 0;
}

static unsigned int next_server_seq(void){
    g_sess.server_next_seq++;
    if (g_sess.server_next_seq == 0u){
        g_sess.server_next_seq = 1u;
    }
    return g_sess.server_next_seq;
}

static void make_iv(unsigned int seq, unsigned char out_iv[12]){
    for (unsigned int i = 0; i < 12u; i++){
        out_iv[i] = g_sess.nonce_base[i];
    }
    out_iv[8] ^= (unsigned char)((seq >> 24) & 0xFFu);
    out_iv[9] ^= (unsigned char)((seq >> 16) & 0xFFu);
    out_iv[10] ^= (unsigned char)((seq >> 8) & 0xFFu);
    out_iv[11] ^= (unsigned char)(seq & 0xFFu);
}

static int parse_client_kex_ext(const unsigned char* payload,
                                unsigned short payload_len,
                                unsigned int off,
                                unsigned char* out_req_mode,
                                unsigned short* out_mlkem_pk_len){
    unsigned char req_mode;
    unsigned short mlkem_pk_len;

    if (!payload || !out_req_mode || !out_mlkem_pk_len){
        return -1;
    }
    *out_req_mode = RLOGIN_KEX_X25519;
    *out_mlkem_pk_len = 0u;

    if (off == (unsigned int)payload_len){
        return 0;
    }
    if ((unsigned int)payload_len < off + RLOGIN_KEX_EXT_FIXED_LEN){
        return -1;
    }
    if (read_be32(&payload[off]) != RLOGIN_KEX_EXT_MAGIC){
        return -1;
    }
    if (payload[off + 4u] != RLOGIN_KEX_EXT_VER){
        return -1;
    }
    req_mode = payload[off + 5u];
    if (!kex_mode_valid(req_mode)){
        return -1;
    }
    mlkem_pk_len = read_be16(&payload[off + 6u]);
    if ((unsigned int)payload_len != off + RLOGIN_KEX_EXT_FIXED_LEN + (unsigned int)mlkem_pk_len){
        return -1;
    }
    if (req_mode == RLOGIN_KEX_X25519){
        if (mlkem_pk_len != 0u){
            return -1;
        }
    } else{
        if (mlkem_pk_len != (unsigned short)QOS_KEM_MLKEM768_PK_BYTES){
            return -1;
        }
    }
    *out_req_mode = req_mode;
    *out_mlkem_pk_len = mlkem_pk_len;
    return 0;
}

static int derive_session_keys(unsigned char kex_mode,
                               const unsigned char x_shared[32],
                               int have_x,
                               const unsigned char pq_shared[QOS_KEM_MLKEM768_SS_BYTES],
                               int have_pq){
    static const unsigned char key_label[] = "qos-rlogin-key-v1";
    static const unsigned char iv_label[] = "qos-rlogin-iv-v1";
    unsigned char ikm[64];
    unsigned int ikm_len = 0u;
    unsigned char salt_buf[64];
    unsigned char prk[32];
    int rc = -1;

    if (!kex_mode_valid(kex_mode)){
        return -1;
    }
    if (kex_mode == RLOGIN_KEX_X25519){
        if (!have_x || !x_shared){
            return -1;
        }
        for (unsigned int i = 0; i < 32u; i++){
            ikm[ikm_len++] = x_shared[i];
        }
    } else if (kex_mode == RLOGIN_KEX_MLKEM768){
        if (!have_pq || !pq_shared){
            return -1;
        }
        for (unsigned int i = 0; i < QOS_KEM_MLKEM768_SS_BYTES; i++){
            ikm[ikm_len++] = pq_shared[i];
        }
    } else{
        if (!have_x || !x_shared || !have_pq || !pq_shared){
            return -1;
        }
        for (unsigned int i = 0; i < 32u; i++){
            ikm[ikm_len++] = x_shared[i];
        }
        for (unsigned int i = 0; i < QOS_KEM_MLKEM768_SS_BYTES; i++){
            ikm[ikm_len++] = pq_shared[i];
        }
    }

    for (unsigned int i = 0; i < 32u; i++){
        salt_buf[i] = g_sess.client_nonce[i];
        salt_buf[32u + i] = g_sess.server_nonce[i];
    }

    crypto_hkdf_sha256_extract(salt_buf, sizeof(salt_buf), ikm, ikm_len, prk);
    if (crypto_hkdf_sha256_expand(prk,
                                  key_label,
                                  (unsigned int)(sizeof(key_label) - 1u),
                                  g_sess.key, 32u) != 0){
        goto out;
    }
    if (crypto_hkdf_sha256_expand(prk,
                                  iv_label,
                                  (unsigned int)(sizeof(iv_label) - 1u),
                                  g_sess.nonce_base, 12u) != 0){
        goto out;
    }
    g_sess.kex_mode = kex_mode;
    rc = 0;

out:
    crypto_memzero(ikm, sizeof(ikm));
    crypto_memzero(salt_buf, sizeof(salt_buf));
    crypto_memzero(prk, sizeof(prk));
    return rc;
}

static int send_plain(unsigned char type,
                      unsigned int session_id,
                      unsigned int seq,
                      const unsigned char* payload,
                      unsigned short payload_len){
    unsigned char out[20 + RLOGIN_PAYLOAD_MAX];
    int hlen = build_header(out, type, session_id, seq, payload_len);
    if (hlen < 0 || payload_len > RLOGIN_PAYLOAD_MAX){
        return -1;
    }
    for (unsigned int i = 0; i < payload_len; i++){
        out[20u + i] = payload ? payload[i] : 0;
    }
    if (udp_send(g_sess.src_ip, RLOGIN_PORT, g_sess.src_port, out, 20u + payload_len) != 0){
        headless_control_note_remote_login_tx_fail();
        return -1;
    }
    g_stats.tx_total++;
    headless_control_note_remote_login_tx();
    return 0;
}

static int send_encrypted(unsigned char type,
                          unsigned int session_id,
                          unsigned int seq,
                          const unsigned char* plain,
                          unsigned short plain_len){
    unsigned char out[20 + RLOGIN_PAYLOAD_MAX];
    unsigned char iv[12];
    unsigned char tag[RLOGIN_TAG_LEN];
    aes_gcm_key_t key;
    int hlen;
    int rc;

    if (plain_len + RLOGIN_TAG_LEN > RLOGIN_PAYLOAD_MAX){
        return -1;
    }
    hlen = build_header(out, type, session_id, seq, (unsigned short)(plain_len + RLOGIN_TAG_LEN));
    if (hlen < 0){
        return -1;
    }
    if (aes_gcm_key_init(&key, g_sess.key, 32u) != 0){
        return -1;
    }

    make_iv(seq, iv);
    rc = aes_gcm_encrypt(&key, iv, sizeof(iv),
                         out, 20u,
                         plain, plain_len,
                         &out[20u], tag, RLOGIN_TAG_LEN);
    crypto_memzero(&key, sizeof(key));
    if (rc != 0){
        return -1;
    }
    for (unsigned int i = 0; i < RLOGIN_TAG_LEN; i++){
        out[20u + plain_len + i] = tag[i];
    }
    if (udp_send(g_sess.src_ip, RLOGIN_PORT, g_sess.src_port, out, 20u + plain_len + RLOGIN_TAG_LEN) != 0){
        headless_control_note_remote_login_tx_fail();
        return -1;
    }
    g_stats.tx_total++;
    headless_control_note_remote_login_tx();
    return 0;
}

static int decrypt_payload(const unsigned char* frame,
                           unsigned int frame_len,
                           unsigned int seq,
                           unsigned char* out_plain,
                           unsigned short out_plain_cap,
                           unsigned short* out_plain_len){
    unsigned short payload_len = read_be16(&frame[16]);
    if (frame_len < 20u || payload_len < RLOGIN_TAG_LEN){
        return -1;
    }
    unsigned int ct_len = (unsigned int)payload_len - RLOGIN_TAG_LEN;
    const unsigned char* ct = &frame[20];
    const unsigned char* tag = &frame[20u + ct_len];
    aes_gcm_key_t key;
    unsigned char iv[12];

    if (ct_len > out_plain_cap){
        return -1;
    }

    if (aes_gcm_key_init(&key, g_sess.key, 32u) != 0){
        return -1;
    }
    make_iv(seq, iv);
    int rc = aes_gcm_decrypt(&key, iv, sizeof(iv),
                             frame, 20u,
                             ct, ct_len,
                             out_plain,
                             tag, RLOGIN_TAG_LEN);
    crypto_memzero(&key, sizeof(key));
    if (rc != 0){
        return -1;
    }
    *out_plain_len = (unsigned short)ct_len;
    return 0;
}

static int tty_in_enqueue(const unsigned char* data, unsigned int len){
    unsigned int pushed = 0;
    QOS_ASSERT(g_tty_in_head < RLOGIN_TTY_IN_CAP);
    QOS_ASSERT(g_tty_in_tail < RLOGIN_TTY_IN_CAP);
    QOS_ASSERT(g_tty_in_count <= RLOGIN_TTY_IN_CAP);
    for (unsigned int i = 0; i < len; i++){
        if (g_tty_in_count >= RLOGIN_TTY_IN_CAP){
            break;
        }
        g_tty_in_q[g_tty_in_tail] = data[i];
        g_tty_in_tail = (g_tty_in_tail + 1u) % RLOGIN_TTY_IN_CAP;
        g_tty_in_count++;
        pushed++;
    }
    return (int)pushed;
}

static int tty_out_enqueue_char(unsigned char c){
    QOS_ASSERT(g_tty_out_head < RLOGIN_TTY_OUT_CAP);
    QOS_ASSERT(g_tty_out_tail < RLOGIN_TTY_OUT_CAP);
    QOS_ASSERT(g_tty_out_count <= RLOGIN_TTY_OUT_CAP);
    if (g_tty_out_count >= RLOGIN_TTY_OUT_CAP){
        return -1;
    }
    g_tty_out_q[g_tty_out_tail] = c;
    g_tty_out_tail = (g_tty_out_tail + 1u) % RLOGIN_TTY_OUT_CAP;
    g_tty_out_count++;
    return 0;
}

static void send_reject_server_hello(const udp_meta_t* meta, unsigned char status){
    unsigned char resp[RLOGIN_SERVER_HELLO_MAX_LEN];
    auth_kdf_info_t kdf_info;
    unsigned int off;
    unsigned int resp_len = RLOGIN_SERVER_HELLO_BASE_LEN + RLOGIN_SERVER_HELLO_KDF_EXT_LEN;
    unsigned char sid_src[4];

    for (unsigned int i = 0; i < sizeof(resp); i++){
        resp[i] = 0;
    }
    resp[32u + AUTH_NONCE_BYTES + AUTH_SALT_BYTES] = status;
    if (auth_get_kdf_info(&kdf_info) != 0){
        kdf_info.kdf_id = AUTH_KDF_SHA256;
        kdf_info.argon2_t_cost = AUTH_ARGON2_DEFAULT_T_COST;
        kdf_info.argon2_m_cost_kib = AUTH_ARGON2_DEFAULT_M_COST_KIB;
        kdf_info.argon2_parallelism = AUTH_ARGON2_DEFAULT_PARALLELISM;
        kdf_info.argon2_version = AUTH_ARGON2_DEFAULT_VERSION;
    }
    off = RLOGIN_SERVER_HELLO_BASE_LEN;
    resp[off++] = kdf_info.kdf_id;
    write_be32(&resp[off], kdf_info.argon2_t_cost);
    off += 4u;
    write_be32(&resp[off], kdf_info.argon2_m_cost_kib);
    off += 4u;
    write_be32(&resp[off], kdf_info.argon2_parallelism);
    off += 4u;
    write_be32(&resp[off], kdf_info.argon2_version);
    off += 4u;
    write_be32(&resp[off], RLOGIN_KEX_EXT_MAGIC);
    off += 4u;
    resp[off++] = RLOGIN_KEX_EXT_VER;
    resp[off++] = RLOGIN_KEX_X25519;
    write_be16(&resp[off], 0u);
    off += 2u;
    resp_len = off;

    session_clear();
    session_copy_peer(meta);
    if (crypto_random_bytes(sid_src, sizeof(sid_src)) == 0){
        g_sess.session_id = read_be32(sid_src);
    } else{
        g_sess.session_id = (unsigned int)system_ticks ^ ((unsigned int)meta->src_port << 16) ^ 0x51524C47u;
    }
    if (g_sess.session_id == 0u){
        g_sess.session_id = 1u;
    }
    (void)send_plain(RLOGIN_TYPE_SERVER_HELLO, g_sess.session_id, next_server_seq(), resp, (unsigned short)resp_len);
    session_clear();
}

static void handle_client_hello(const unsigned char* payload, unsigned short payload_len, const udp_meta_t* meta){
    unsigned char resp[RLOGIN_SERVER_HELLO_MAX_LEN];
    unsigned char rnd[8];
    auth_kdf_info_t kdf_info;
    unsigned int off = 0;
    unsigned int resp_len = RLOGIN_SERVER_HELLO_BASE_LEN + RLOGIN_SERVER_HELLO_KDF_EXT_LEN;
    unsigned int username_len;
    unsigned char salt[AUTH_SALT_BYTES];
    int status = 0;
    unsigned char req_kex_mode = RLOGIN_KEX_X25519;
    unsigned short client_mlkem_pk_len = 0u;
    unsigned char selected_kex_mode = RLOGIN_KEX_X25519;
    unsigned short server_mlkem_ct_len = 0u;
    unsigned char x_shared[32];
    unsigned char pq_shared[QOS_KEM_MLKEM768_SS_BYTES];
    int have_x_shared = 0;
    int have_pq_shared = 0;
    int fail = 0;

    if (payload_len > RLOGIN_PAYLOAD_MAX){
        g_stats.bad_header++;
        return;
    }
    if (payload_len < (32u + AUTH_NONCE_BYTES + 1u)){
        g_stats.bad_header++;
        return;
    }

    if (!g_auth_ready){
        send_reject_server_hello(meta, RLOGIN_STATUS_AUTH_UNAVAILABLE);
        return;
    }
    {
        unsigned char gate = auth_gate_status();
        if (gate == RLOGIN_STATUS_LOCKED){
            g_stats.auth_locked++;
            send_reject_server_hello(meta, RLOGIN_STATUS_LOCKED);
            return;
        }
        if (gate == RLOGIN_STATUS_RATE_LIMITED){
            g_stats.auth_rate_limited++;
            send_reject_server_hello(meta, RLOGIN_STATUS_RATE_LIMITED);
            return;
        }
    }

    session_clear();
    session_copy_peer(meta);

    for (unsigned int i = 0; i < 32u; i++){
        g_sess.client_pub[i] = payload[off + i];
    }
    off += 32u;
    for (unsigned int i = 0; i < AUTH_NONCE_BYTES; i++){
        g_sess.client_nonce[i] = payload[off + i];
    }
    off += AUTH_NONCE_BYTES;

    username_len = payload[off++];
    if (username_len == 0u || username_len > AUTH_USERNAME_MAX || off + username_len > payload_len){
        g_stats.bad_header++;
        return;
    }

    for (unsigned int i = 0; i < username_len; i++){
        g_sess.username[i] = (char)payload[off + i];
    }
    g_sess.username[username_len] = 0;
    if (parse_client_kex_ext(payload, payload_len, off + username_len, &req_kex_mode, &client_mlkem_pk_len) != 0){
        g_stats.bad_header++;
        fail = 1;
        goto out;
    }
    if (client_mlkem_pk_len == (unsigned short)QOS_KEM_MLKEM768_PK_BYTES){
        unsigned int pk_off = off + username_len + RLOGIN_KEX_EXT_FIXED_LEN;
        for (unsigned int i = 0; i < QOS_KEM_MLKEM768_PK_BYTES; i++){
            g_sess.client_mlkem_pk[i] = payload[pk_off + i];
        }
    }

    if (x25519_generate_keypair(g_sess.server_priv, g_sess.server_pub) != 0){
        g_stats.bad_crypto++;
        fail = 1;
        goto out;
    }
    if (x25519_shared_secret(g_sess.server_priv, g_sess.client_pub, x_shared) == 0 &&
        !x25519_is_all_zero(x_shared)){
        have_x_shared = 1;
    }
    if (auth_issue_nonce(g_sess.server_nonce) != 0){
        g_stats.bad_crypto++;
        fail = 1;
        goto out;
    }
    if (crypto_random_bytes(rnd, sizeof(rnd)) != 0){
        for (unsigned int i = 0; i < sizeof(rnd); i++){
            rnd[i] = (unsigned char)(i * 37u + 11u);
        }
    }
    g_sess.session_id = read_be32(&rnd[0]) ^ read_be32(&rnd[4]);
    if (g_sess.session_id == 0u){
        g_sess.session_id = 1u;
    }
    if ((req_kex_mode == RLOGIN_KEX_MLKEM768 || req_kex_mode == RLOGIN_KEX_HYBRID) &&
        client_mlkem_pk_len == (unsigned short)QOS_KEM_MLKEM768_PK_BYTES){
        if (pq_kem_mlkem768_available() &&
            pq_kem_mlkem768_encaps(g_sess.server_mlkem_ct, sizeof(g_sess.server_mlkem_ct),
                                   pq_shared, sizeof(pq_shared),
                                   g_sess.client_mlkem_pk, sizeof(g_sess.client_mlkem_pk)) == 0){
            have_pq_shared = 1;
            server_mlkem_ct_len = (unsigned short)QOS_KEM_MLKEM768_CT_BYTES;
        }
    }

    if (req_kex_mode == RLOGIN_KEX_X25519){
        if (!have_x_shared){
            g_stats.bad_crypto++;
            fail = 1;
            goto out;
        }
        selected_kex_mode = RLOGIN_KEX_X25519;
    } else if (req_kex_mode == RLOGIN_KEX_MLKEM768){
        if (!have_pq_shared){
            g_stats.bad_crypto++;
            fail = 1;
            goto out;
        }
        selected_kex_mode = RLOGIN_KEX_MLKEM768;
    } else{
        if (!have_x_shared || !have_pq_shared){
            g_stats.bad_crypto++;
            fail = 1;
            goto out;
        }
        selected_kex_mode = RLOGIN_KEX_HYBRID;
    }

    if (derive_session_keys(selected_kex_mode, x_shared, have_x_shared, pq_shared, have_pq_shared) != 0){
        g_stats.bad_crypto++;
        fail = 1;
        goto out;
    }

    if (auth_get_salt(salt) != 0){
        status = 1;
        for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
            salt[i] = 0;
        }
    }
    if (auth_get_kdf_info(&kdf_info) != 0){
        status = 1;
        kdf_info.kdf_id = AUTH_KDF_SHA256;
        kdf_info.argon2_t_cost = AUTH_ARGON2_DEFAULT_T_COST;
        kdf_info.argon2_m_cost_kib = AUTH_ARGON2_DEFAULT_M_COST_KIB;
        kdf_info.argon2_parallelism = AUTH_ARGON2_DEFAULT_PARALLELISM;
        kdf_info.argon2_version = AUTH_ARGON2_DEFAULT_VERSION;
    }

    for (unsigned int i = 0; i < 32u; i++){
        resp[i] = g_sess.server_pub[i];
    }
    for (unsigned int i = 0; i < AUTH_NONCE_BYTES; i++){
        resp[32u + i] = g_sess.server_nonce[i];
    }
    for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
        resp[32u + AUTH_NONCE_BYTES + i] = salt[i];
    }
    resp[32u + AUTH_NONCE_BYTES + AUTH_SALT_BYTES] = (unsigned char)status;
    off = RLOGIN_SERVER_HELLO_BASE_LEN;
    resp[off++] = kdf_info.kdf_id;
    write_be32(&resp[off], kdf_info.argon2_t_cost);
    off += 4u;
    write_be32(&resp[off], kdf_info.argon2_m_cost_kib);
    off += 4u;
    write_be32(&resp[off], kdf_info.argon2_parallelism);
    off += 4u;
    write_be32(&resp[off], kdf_info.argon2_version);
    off += 4u;
    write_be32(&resp[off], RLOGIN_KEX_EXT_MAGIC);
    off += 4u;
    resp[off++] = RLOGIN_KEX_EXT_VER;
    resp[off++] = selected_kex_mode;
    write_be16(&resp[off], server_mlkem_ct_len);
    off += 2u;
    for (unsigned int i = 0; i < server_mlkem_ct_len; i++){
        resp[off + i] = g_sess.server_mlkem_ct[i];
    }
    off += server_mlkem_ct_len;
    resp_len = off;

    g_sess.active = 1;
    g_sess.authed = 0;
    g_sess.tty_attached = 0;
    g_sess.client_seq_top = 0;
    g_sess.client_seq_seen_bitmap = 0;
    g_sess.server_next_seq = 0;
    if (selected_kex_mode == RLOGIN_KEX_HYBRID){
        g_stats.kex_hybrid++;
    } else if (selected_kex_mode == RLOGIN_KEX_MLKEM768){
        g_stats.kex_mlkem768++;
    } else{
        g_stats.kex_x25519++;
    }
    (void)send_plain(RLOGIN_TYPE_SERVER_HELLO, g_sess.session_id, next_server_seq(), resp, (unsigned short)resp_len);
out:
    crypto_memzero(x_shared, sizeof(x_shared));
    crypto_memzero(pq_shared, sizeof(pq_shared));
    if (fail){
        session_clear();
    }
}

static void handle_auth_proof(const unsigned char* frame,
                              unsigned int frame_len,
                              unsigned int session_id,
                              unsigned int seq,
                              const udp_meta_t* meta){
    unsigned char plain[64];
    unsigned short plain_len = 0;
    unsigned char result = 0;

    if (!g_sess.active || g_sess.authed){
        return;
    }
    if (session_id != g_sess.session_id){
        g_stats.bad_header++;
        return;
    }
    if (!client_seq_is_fresh(seq)){
        g_stats.bad_header++;
        g_stats.replay_drop++;
        return;
    }
    if (!ip4_eq(meta->src_ip, g_sess.src_ip) || meta->src_port != g_sess.src_port){
        g_stats.bad_header++;
        return;
    }
    if (decrypt_payload(frame, frame_len, seq, plain, (unsigned short)sizeof(plain), &plain_len) != 0){
        g_stats.bad_crypto++;
        return;
    }
    if (plain_len != AUTH_HASH_BYTES){
        g_stats.bad_header++;
        return;
    }

    client_seq_mark_seen(seq);
    if (auth_verify_response(g_sess.username, g_sess.client_nonce, g_sess.server_nonce, plain) == 0){
        g_sess.authed = 1;
        g_sess.tty_attached = 1;
        result = 1u;
        g_stats.auth_ok++;
        auth_mark_success();
        headless_control_note_login_success();
    } else{
        g_stats.auth_fail++;
        auth_mark_failure();
    }

    (void)send_encrypted(RLOGIN_TYPE_AUTH_RESULT, g_sess.session_id, next_server_seq(), &result, 1u);
    if (!g_sess.authed){
        session_clear();
    }
}

static void handle_command(const unsigned char* frame,
                           unsigned int frame_len,
                           unsigned int session_id,
                           unsigned int seq,
                           const udp_meta_t* meta){
    unsigned char plain[160];
    unsigned short plain_len = 0;
    unsigned char resp[192];
    unsigned int n = 0;

    if (!g_sess.active || !g_sess.authed){
        return;
    }
    if (session_id != g_sess.session_id){
        g_stats.bad_header++;
        return;
    }
    if (!client_seq_is_fresh(seq)){
        g_stats.bad_header++;
        g_stats.replay_drop++;
        return;
    }
    if (!ip4_eq(meta->src_ip, g_sess.src_ip) || meta->src_port != g_sess.src_port){
        g_stats.bad_header++;
        return;
    }
    if (decrypt_payload(frame, frame_len, seq, plain, (unsigned short)sizeof(plain), &plain_len) != 0){
        g_stats.bad_crypto++;
        return;
    }

    client_seq_mark_seen(seq);
    if (plain_len == 4u &&
        plain[0] == 'p' && plain[1] == 'i' && plain[2] == 'n' && plain[3] == 'g'){
        resp[0] = 'p'; resp[1] = 'o'; resp[2] = 'n'; resp[3] = 'g'; resp[4] = '\n';
        n = 5u;
        g_stats.cmd_ok++;
    } else if (plain_len == 6u &&
               plain[0] == 's' && plain[1] == 't' && plain[2] == 'a' &&
               plain[3] == 't' && plain[4] == 'u' && plain[5] == 's'){
        static const char k_msg[] = "remote auth ok; shell tunnel active\n";
        for (unsigned int i = 0; i < (unsigned int)(sizeof(k_msg) - 1u); i++){
            resp[i] = (unsigned char)k_msg[i];
        }
        n = (unsigned int)(sizeof(k_msg) - 1u);
        g_stats.cmd_ok++;
    } else if (plain_len == 6u &&
               plain[0] == 'a' && plain[1] == 't' && plain[2] == 't' &&
               plain[3] == 'a' && plain[4] == 'c' && plain[5] == 'h'){
        g_sess.tty_attached = 1;
        static const char k_msg[] = "tty attached\n";
        for (unsigned int i = 0; i < (unsigned int)(sizeof(k_msg) - 1u); i++){
            resp[i] = (unsigned char)k_msg[i];
        }
        n = (unsigned int)(sizeof(k_msg) - 1u);
        g_stats.cmd_ok++;
    } else if (plain_len == 6u &&
               plain[0] == 'd' && plain[1] == 'e' && plain[2] == 't' &&
               plain[3] == 'a' && plain[4] == 'c' && plain[5] == 'h'){
        g_sess.tty_attached = 0;
        static const char k_msg[] = "tty detached\n";
        for (unsigned int i = 0; i < (unsigned int)(sizeof(k_msg) - 1u); i++){
            resp[i] = (unsigned char)k_msg[i];
        }
        n = (unsigned int)(sizeof(k_msg) - 1u);
        g_stats.cmd_ok++;
    } else{
        static const char k_msg[] = "unknown command\n";
        for (unsigned int i = 0; i < (unsigned int)(sizeof(k_msg) - 1u); i++){
            resp[i] = (unsigned char)k_msg[i];
        }
        n = (unsigned int)(sizeof(k_msg) - 1u);
        g_stats.cmd_fail++;
    }
    (void)send_encrypted(RLOGIN_TYPE_COMMAND_RESULT, g_sess.session_id, next_server_seq(), resp, (unsigned short)n);
}

static void handle_tty_input(const unsigned char* frame,
                             unsigned int frame_len,
                             unsigned int session_id,
                             unsigned int seq,
                             const udp_meta_t* meta){
    unsigned char plain[RLOGIN_PAYLOAD_MAX];
    unsigned short plain_len = 0;
    if (!g_sess.active || !g_sess.authed || !g_sess.tty_attached){
        return;
    }
    if (session_id != g_sess.session_id){
        g_stats.bad_header++;
        return;
    }
    if (!client_seq_is_fresh(seq)){
        g_stats.bad_header++;
        g_stats.replay_drop++;
        return;
    }
    if (!ip4_eq(meta->src_ip, g_sess.src_ip) || meta->src_port != g_sess.src_port){
        g_stats.bad_header++;
        return;
    }
    if (decrypt_payload(frame, frame_len, seq, plain, (unsigned short)sizeof(plain), &plain_len) != 0){
        g_stats.bad_crypto++;
        return;
    }
    client_seq_mark_seen(seq);
    int pushed = tty_in_enqueue(plain, plain_len);
    if (pushed > 0){
        g_stats.tty_in_bytes += (unsigned long)pushed;
    }
}

static void flush_tty_output_once(void){
    unsigned char payload[RLOGIN_TTY_OUT_CHUNK];
    unsigned int n = 0;
    QOS_ASSERT(g_tty_out_head < RLOGIN_TTY_OUT_CAP);
    QOS_ASSERT(g_tty_out_tail < RLOGIN_TTY_OUT_CAP);
    QOS_ASSERT(g_tty_out_count <= RLOGIN_TTY_OUT_CAP);
    if (!g_sess.active || !g_sess.authed || !g_sess.tty_attached || g_tty_out_count == 0){
        return;
    }
    while (n < RLOGIN_TTY_OUT_CHUNK && g_tty_out_count > 0){
        payload[n++] = g_tty_out_q[g_tty_out_head];
        g_tty_out_head = (g_tty_out_head + 1u) % RLOGIN_TTY_OUT_CAP;
        g_tty_out_count--;
    }
    if (n == 0u){
        return;
    }
    if (send_encrypted(RLOGIN_TYPE_TTY_OUTPUT, g_sess.session_id, next_server_seq(), payload, (unsigned short)n) != 0){
        /* On send failure, drop chunk to keep kernel non-blocking. */
    } else{
        g_stats.tty_out_bytes += (unsigned long)n;
    }
}

int remote_login_init(void){
    session_clear();
    g_stats.rx_total = 0;
    g_stats.tx_total = 0;
    g_stats.bad_header = 0;
    g_stats.bad_crypto = 0;
    g_stats.auth_ok = 0;
    g_stats.auth_fail = 0;
    g_stats.auth_rate_limited = 0;
    g_stats.auth_locked = 0;
    g_stats.replay_drop = 0;
    g_stats.cmd_ok = 0;
    g_stats.cmd_fail = 0;
    g_stats.tty_in_bytes = 0;
    g_stats.tty_out_bytes = 0;
    g_stats.kex_x25519 = 0;
    g_stats.kex_mlkem768 = 0;
    g_stats.kex_hybrid = 0;
    g_auth_fail_streak = 0;
    g_auth_delay_until_tick = 0;
    g_auth_lockout_until_tick = 0;

    g_enabled = 1;
    g_auth_ready = auth_is_ready() ? 1 : 0;
    uart_puts("Remote login: enabled on UDP port 2222\n");
    uart_puts("Remote login: ML-KEM backend ");
    uart_puts(pq_kem_mlkem768_available() ? "available\n" : "unavailable (X25519 only)\n");
#if defined(QOS_BOARD_PI_ZERO2W) && QOS_BOARD_PI_ZERO2W
    rlogin_headless_write("Pi0 remote login: enabled UDP 2222\n");
    rlogin_headless_write("Pi0 remote login: ML-KEM ");
    rlogin_headless_write(pq_kem_mlkem768_available() ? "available\n" : "unavailable; X25519 fallback\n");
#endif
    if (!g_auth_ready){
        uart_puts("Remote login: AUTH.BIN not ready; auth attempts will be rejected\n");
#if defined(QOS_BOARD_PI_ZERO2W) && QOS_BOARD_PI_ZERO2W
        rlogin_headless_write("Pi0 remote login: AUTH.BIN not ready\n");
#endif
        return -1;
    }
    return 0;
}

int remote_login_enabled(void){
    return g_enabled;
}

int remote_login_try_read_tty_char(char* out){
    if (!out || !g_enabled || !g_sess.active || !g_sess.authed || !g_sess.tty_attached || g_tty_in_count == 0){
        return 0;
    }
    *out = (char)g_tty_in_q[g_tty_in_head];
    g_tty_in_head = (g_tty_in_head + 1u) % RLOGIN_TTY_IN_CAP;
    g_tty_in_count--;
    return 1;
}

void remote_login_on_tty_output_char(char c){
    if (!g_enabled || !g_sess.active || !g_sess.authed || !g_sess.tty_attached){
        return;
    }
    (void)tty_out_enqueue_char((unsigned char)c);
}

void remote_login_poll(void){
    static unsigned char frame[20 + RLOGIN_PAYLOAD_MAX];
    udp_meta_t meta;
    int n;
    int loops = 0;

    if (!g_enabled){
        return;
    }
    rlogin_headless_diag_tick();

    while (loops++ < 6){
        n = udp_recv_filtered(RLOGIN_PORT, 0, 0, 0, frame, sizeof(frame), &meta);
        if (n <= 0){
            break;
        }
        if ((unsigned int)n > sizeof(frame)){
            g_stats.bad_header++;
            continue;
        }
        g_stats.rx_total++;
        headless_control_note_remote_login_rx();

        unsigned char type = 0;
        unsigned int session_id = 0;
        unsigned int seq = 0;
        unsigned short payload_len = 0;
        if (parse_header(frame, (unsigned int)n, &type, &session_id, &seq, &payload_len) != 0){
            g_stats.bad_header++;
            continue;
        }
        const unsigned char* payload = &frame[20];

        if (type == RLOGIN_TYPE_CLIENT_HELLO){
            handle_client_hello(payload, payload_len, &meta);
        } else if (type == RLOGIN_TYPE_AUTH_PROOF){
            handle_auth_proof(frame, (unsigned int)n, session_id, seq, &meta);
        } else if (type == RLOGIN_TYPE_COMMAND){
            handle_command(frame, (unsigned int)n, session_id, seq, &meta);
        } else if (type == RLOGIN_TYPE_TTY_INPUT){
            handle_tty_input(frame, (unsigned int)n, session_id, seq, &meta);
        } else{
            g_stats.bad_header++;
        }
    }

    {
        int tx_loops = 0;
        while (tx_loops++ < 4 && g_tty_out_count > 0){
            flush_tty_output_once();
        }
    }
}

void remote_login_dump_stats(void){
    uart_puts("RLOGIN state en=");
    uart_putdec((unsigned long)g_enabled);
    uart_puts(" auth=");
    uart_putdec((unsigned long)g_auth_ready);
    uart_puts(" active=");
    uart_putdec((unsigned long)g_sess.active);
    uart_puts(" authed=");
    uart_putdec((unsigned long)g_sess.authed);
    uart_puts(" tty=");
    uart_putdec((unsigned long)g_sess.tty_attached);
    uart_puts(" inq=");
    uart_putdec((unsigned long)g_tty_in_count);
    uart_puts(" outq=");
    uart_putdec((unsigned long)g_tty_out_count);
    uart_puts(" kex=");
    uart_puts(kex_mode_name(g_sess.kex_mode));
    uart_puts("\n");

    uart_puts("RLOGIN rx=");
    uart_putdec(g_stats.rx_total);
    uart_puts(" tx=");
    uart_putdec(g_stats.tx_total);
    uart_puts(" auth_ok=");
    uart_putdec(g_stats.auth_ok);
    uart_puts(" auth_fail=");
    uart_putdec(g_stats.auth_fail);
    uart_puts(" auth_rl=");
    uart_putdec(g_stats.auth_rate_limited);
    uart_puts(" auth_lock=");
    uart_putdec(g_stats.auth_locked);
    uart_puts(" replay_drop=");
    uart_putdec(g_stats.replay_drop);
    uart_puts(" bad_hdr=");
    uart_putdec(g_stats.bad_header);
    uart_puts(" bad_crypto=");
    uart_putdec(g_stats.bad_crypto);
    uart_puts(" cmd_ok=");
    uart_putdec(g_stats.cmd_ok);
    uart_puts(" cmd_fail=");
    uart_putdec(g_stats.cmd_fail);
    uart_puts(" tty_in=");
    uart_putdec(g_stats.tty_in_bytes);
    uart_puts(" tty_out=");
    uart_putdec(g_stats.tty_out_bytes);
    uart_puts(" kex_x=");
    uart_putdec(g_stats.kex_x25519);
    uart_puts(" kex_pq=");
    uart_putdec(g_stats.kex_mlkem768);
    uart_puts(" kex_hybrid=");
    uart_putdec(g_stats.kex_hybrid);
    uart_puts("\n");
}

unsigned int remote_login_state_bits(void){
    unsigned int bits = 0u;
    if (g_enabled){
        bits |= 1u;      // enabled
    }
    if (g_sess.active){
        bits |= 1u << 1; // active session
    }
    if (g_sess.authed){
        bits |= 1u << 2; // authenticated
    }
    if (g_sess.tty_attached){
        bits |= 1u << 3; // tty attached
    }
    return bits;
}

void remote_login_get_diag(unsigned long* rx,
                           unsigned long* tx,
                           unsigned long* bad_header,
                           unsigned long* bad_crypto){
    if (rx){
        *rx = g_stats.rx_total;
    }
    if (tx){
        *tx = g_stats.tx_total;
    }
    if (bad_header){
        *bad_header = g_stats.bad_header;
    }
    if (bad_crypto){
        *bad_crypto = g_stats.bad_crypto;
    }
}
