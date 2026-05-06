#include "remote_login.h"
#include "auth.h"
#include "udp.h"
#include "x25519.h"
#include "sha256.h"
#include "crypto.h"
#include "aes_gcm.h"
#include "uart.h"

#define RLOGIN_PORT 2222u
#define RLOGIN_MAGIC 0x51524C47u /* QRLG */
#define RLOGIN_VER 1u

#define RLOGIN_TYPE_CLIENT_HELLO 1u
#define RLOGIN_TYPE_SERVER_HELLO 2u
#define RLOGIN_TYPE_AUTH_PROOF 3u
#define RLOGIN_TYPE_AUTH_RESULT 4u
#define RLOGIN_TYPE_COMMAND 5u
#define RLOGIN_TYPE_COMMAND_RESULT 6u

#define RLOGIN_PAYLOAD_MAX 512u
#define RLOGIN_TAG_LEN 16u

typedef struct {
    unsigned long rx_total;
    unsigned long tx_total;
    unsigned long bad_header;
    unsigned long bad_crypto;
    unsigned long auth_ok;
    unsigned long auth_fail;
    unsigned long cmd_ok;
    unsigned long cmd_fail;
} remote_login_stats_t;

typedef struct {
    int active;
    int authed;
    unsigned char src_ip[4];
    unsigned short src_port;
    unsigned int session_id;
    unsigned int last_seq;
    char username[AUTH_USERNAME_MAX + 1u];
    unsigned char client_pub[32];
    unsigned char server_priv[32];
    unsigned char server_pub[32];
    unsigned char client_nonce[AUTH_NONCE_BYTES];
    unsigned char server_nonce[AUTH_NONCE_BYTES];
    unsigned char key[32];
    unsigned char nonce_base[12];
} remote_login_session_t;

static remote_login_stats_t g_stats;
static remote_login_session_t g_sess;
static int g_enabled = 0;

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

static void session_clear(void){
    g_sess.active = 0;
    g_sess.authed = 0;
    g_sess.src_ip[0] = 0;
    g_sess.src_ip[1] = 0;
    g_sess.src_ip[2] = 0;
    g_sess.src_ip[3] = 0;
    g_sess.src_port = 0;
    g_sess.session_id = 0;
    g_sess.last_seq = 0;
    for (unsigned int i = 0; i < sizeof(g_sess.username); i++){
        g_sess.username[i] = 0;
    }
    crypto_memzero(g_sess.client_pub, sizeof(g_sess.client_pub));
    crypto_memzero(g_sess.server_priv, sizeof(g_sess.server_priv));
    crypto_memzero(g_sess.server_pub, sizeof(g_sess.server_pub));
    crypto_memzero(g_sess.client_nonce, sizeof(g_sess.client_nonce));
    crypto_memzero(g_sess.server_nonce, sizeof(g_sess.server_nonce));
    crypto_memzero(g_sess.key, sizeof(g_sess.key));
    crypto_memzero(g_sess.nonce_base, sizeof(g_sess.nonce_base));
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
    if ((unsigned int)(20u + *out_payload_len) > in_len){
        return -1;
    }
    return 0;
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

static int derive_session_keys(void){
    unsigned char shared[32];
    unsigned char salt_buf[64];
    unsigned char prk[32];
    int rc = -1;

    if (x25519_shared_secret(g_sess.server_priv, g_sess.client_pub, shared) != 0){
        goto out;
    }
    if (x25519_is_all_zero(shared)){
        goto out;
    }

    for (unsigned int i = 0; i < 32u; i++){
        salt_buf[i] = g_sess.client_nonce[i];
        salt_buf[32u + i] = g_sess.server_nonce[i];
    }

    crypto_hkdf_sha256_extract(salt_buf, sizeof(salt_buf), shared, sizeof(shared), prk);
    if (crypto_hkdf_sha256_expand(prk,
                                  (const unsigned char*)"qos-rlogin-key-v1",
                                  16u,
                                  g_sess.key, 32u) != 0){
        goto out;
    }
    if (crypto_hkdf_sha256_expand(prk,
                                  (const unsigned char*)"qos-rlogin-iv-v1",
                                  15u,
                                  g_sess.nonce_base, 12u) != 0){
        goto out;
    }
    rc = 0;

out:
    crypto_memzero(shared, sizeof(shared));
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
        return -1;
    }
    g_stats.tx_total++;
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
        return -1;
    }
    g_stats.tx_total++;
    return 0;
}

static int decrypt_payload(const unsigned char* frame,
                           unsigned int frame_len,
                           unsigned int seq,
                           unsigned char* out_plain,
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

static void handle_client_hello(const unsigned char* payload, unsigned short payload_len, const udp_meta_t* meta){
    unsigned char resp[32 + AUTH_NONCE_BYTES + AUTH_SALT_BYTES + 1u];
    unsigned char rnd[8];
    unsigned int off = 0;
    unsigned int username_len;
    unsigned char salt[AUTH_SALT_BYTES];
    int status = 0;

    if (payload_len < (32u + AUTH_NONCE_BYTES + 1u)){
        g_stats.bad_header++;
        return;
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

    if (x25519_generate_keypair(g_sess.server_priv, g_sess.server_pub) != 0){
        g_stats.bad_crypto++;
        return;
    }
    if (auth_issue_nonce(g_sess.server_nonce) != 0){
        g_stats.bad_crypto++;
        return;
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
    if (derive_session_keys() != 0){
        g_stats.bad_crypto++;
        return;
    }

    if (auth_get_salt(salt) != 0){
        status = 1;
        for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
            salt[i] = 0;
        }
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

    g_sess.active = 1;
    g_sess.authed = 0;
    g_sess.last_seq = 0;
    (void)send_plain(RLOGIN_TYPE_SERVER_HELLO, g_sess.session_id, 0u, resp, sizeof(resp));
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
    if (session_id != g_sess.session_id || seq <= g_sess.last_seq){
        g_stats.bad_header++;
        return;
    }
    if (!ip4_eq(meta->src_ip, g_sess.src_ip) || meta->src_port != g_sess.src_port){
        g_stats.bad_header++;
        return;
    }
    if (decrypt_payload(frame, frame_len, seq, plain, &plain_len) != 0){
        g_stats.bad_crypto++;
        return;
    }
    if (plain_len != AUTH_HASH_BYTES){
        g_stats.bad_header++;
        return;
    }

    if (auth_verify_response(g_sess.username, g_sess.client_nonce, g_sess.server_nonce, plain) == 0){
        g_sess.authed = 1;
        g_sess.last_seq = seq;
        result = 1u;
        g_stats.auth_ok++;
    } else{
        g_stats.auth_fail++;
    }

    (void)send_encrypted(RLOGIN_TYPE_AUTH_RESULT, g_sess.session_id, seq + 1u, &result, 1u);
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
    if (session_id != g_sess.session_id || seq <= g_sess.last_seq){
        g_stats.bad_header++;
        return;
    }
    if (!ip4_eq(meta->src_ip, g_sess.src_ip) || meta->src_port != g_sess.src_port){
        g_stats.bad_header++;
        return;
    }
    if (decrypt_payload(frame, frame_len, seq, plain, &plain_len) != 0){
        g_stats.bad_crypto++;
        return;
    }

    g_sess.last_seq = seq;
    if (plain_len == 4u &&
        plain[0] == 'p' && plain[1] == 'i' && plain[2] == 'n' && plain[3] == 'g'){
        resp[0] = 'p'; resp[1] = 'o'; resp[2] = 'n'; resp[3] = 'g'; resp[4] = '\n';
        n = 5u;
        g_stats.cmd_ok++;
    } else if (plain_len == 6u &&
               plain[0] == 's' && plain[1] == 't' && plain[2] == 'a' &&
               plain[3] == 't' && plain[4] == 'u' && plain[5] == 's'){
        static const char k_msg[] = "remote auth ok; shell tunnel next\n";
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
    (void)send_encrypted(RLOGIN_TYPE_COMMAND_RESULT, g_sess.session_id, seq + 1u, resp, (unsigned short)n);
}

int remote_login_init(void){
    session_clear();
    g_stats.rx_total = 0;
    g_stats.tx_total = 0;
    g_stats.bad_header = 0;
    g_stats.bad_crypto = 0;
    g_stats.auth_ok = 0;
    g_stats.auth_fail = 0;
    g_stats.cmd_ok = 0;
    g_stats.cmd_fail = 0;

    g_enabled = auth_is_ready() ? 1 : 0;
    if (g_enabled){
        uart_puts("Remote login: enabled on UDP port 2222\n");
    } else{
        uart_puts("Remote login: disabled (no AUTH.BIN)\n");
    }
    return g_enabled ? 0 : -1;
}

int remote_login_enabled(void){
    return g_enabled;
}

void remote_login_poll(void){
    static unsigned char frame[20 + RLOGIN_PAYLOAD_MAX];
    udp_meta_t meta;
    int n;
    int loops = 0;

    if (!g_enabled){
        return;
    }

    while (loops++ < 4){
        n = udp_recv_filtered(RLOGIN_PORT, 0, 0, 0, frame, sizeof(frame), &meta);
        if (n <= 0){
            return;
        }
        g_stats.rx_total++;

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
        } else{
            g_stats.bad_header++;
        }
    }
}

void remote_login_dump_stats(void){
    uart_puts("RLOGIN rx=");
    uart_putdec(g_stats.rx_total);
    uart_puts(" tx=");
    uart_putdec(g_stats.tx_total);
    uart_puts(" auth_ok=");
    uart_putdec(g_stats.auth_ok);
    uart_puts(" auth_fail=");
    uart_putdec(g_stats.auth_fail);
    uart_puts(" bad_hdr=");
    uart_putdec(g_stats.bad_header);
    uart_puts(" bad_crypto=");
    uart_putdec(g_stats.bad_crypto);
    uart_puts(" cmd_ok=");
    uart_putdec(g_stats.cmd_ok);
    uart_puts(" cmd_fail=");
    uart_putdec(g_stats.cmd_fail);
    uart_puts("\n");
}
