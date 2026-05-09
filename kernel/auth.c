#include "auth.h"
#include "argon2_kdf.h"
#include "fat32.h"
#include "sha256.h"
#include "crypto.h"
#include "trust.h"
#include "ed25519_verify.h"
#include "pq_sig.h"
#include "uart.h"

#define AUTH_FILE_83 "AUTH    BIN"
#define AUTH_SIG_FILE_83 "AUTH    SIG"
#define AUTH_PQ_FILE_83  "AUTH    PQS"
#define AUTH_SIG_LABEL "AUTH_BIN"
#define AUTH_SIG_LABEL_LEN 8u

#define AUTH_MAGIC_BYTES 8u
#define AUTH_VERSION_V1 1u
#define AUTH_VERSION_V2 2u

#define AUTH_V1_FILE_MIN_BYTES (AUTH_MAGIC_BYTES + 1u + 1u + (AUTH_USERNAME_MAX + 1u) + AUTH_SALT_BYTES + AUTH_HASH_BYTES)
#define AUTH_V2_FILE_MIN_BYTES (AUTH_MAGIC_BYTES + 1u + 1u + (AUTH_USERNAME_MAX + 1u) + 1u + 1u + 4u + 4u + 4u + 4u + AUTH_SALT_BYTES + AUTH_HASH_BYTES)

#define AUTH_ARGON2_MAX_T_COST 10u
#define AUTH_ARGON2_MAX_M_COST_KIB (256u * 1024u)
#define AUTH_ARGON2_MAX_PARALLELISM 4u
#define AUTH_FILE_MAX_BYTES 256u
#define AUTH_SIG_MAX_BYTES 64u
#define AUTH_REQUIRE_ED25519 1
#define AUTH_REQUIRE_PQ 1

typedef struct {
    int ready;
    unsigned char file_version;
    unsigned int username_len;
    char username[AUTH_USERNAME_MAX + 1u];
    unsigned char kdf_id;
    unsigned int argon2_t_cost;
    unsigned int argon2_m_cost_kib;
    unsigned int argon2_parallelism;
    unsigned int argon2_version;
    unsigned char salt[AUTH_SALT_BYTES];
    unsigned char stored_hash[AUTH_HASH_BYTES];
} auth_state_t;

static auth_state_t g_auth;
static unsigned char g_auth_sig[AUTH_SIG_MAX_BYTES];
static unsigned char g_auth_pq_sig[QOS_PQ_SIG_MAX];

static const unsigned char k_auth_magic_v1[AUTH_MAGIC_BYTES] = {
    'Q','A','U','T','H','V','1','\0'
};
static const unsigned char k_auth_magic_v2[AUTH_MAGIC_BYTES] = {
    'Q','A','U','T','H','V','2','\0'
};

static void auth_zero_state(void){
    g_auth.ready = 0;
    g_auth.file_version = 0;
    g_auth.username_len = 0;
    for (unsigned int i = 0; i < sizeof(g_auth.username); i++){
        g_auth.username[i] = 0;
    }
    g_auth.kdf_id = AUTH_KDF_SHA256;
    g_auth.argon2_t_cost = AUTH_ARGON2_DEFAULT_T_COST;
    g_auth.argon2_m_cost_kib = AUTH_ARGON2_DEFAULT_M_COST_KIB;
    g_auth.argon2_parallelism = AUTH_ARGON2_DEFAULT_PARALLELISM;
    g_auth.argon2_version = AUTH_ARGON2_DEFAULT_VERSION;
    crypto_memzero(g_auth.salt, sizeof(g_auth.salt));
    crypto_memzero(g_auth.stored_hash, sizeof(g_auth.stored_hash));
}

static unsigned int cstr_len_bounded(const char* s, unsigned int cap){
    unsigned int n = 0;
    if (!s){
        return 0;
    }
    while (n < cap && s[n]){
        n++;
    }
    return n;
}

static int bytes_eq(const unsigned char* a, const unsigned char* b, unsigned int n){
    unsigned char d = 0;
    for (unsigned int i = 0; i < n; i++){
        d |= (unsigned char)(a[i] ^ b[i]);
    }
    return d == 0;
}

static unsigned int read_le32(const unsigned char* p){
    return ((unsigned int)p[0]) |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static void put_u32_le(unsigned char* out, unsigned int v){
    out[0] = (unsigned char)(v & 0xFFu);
    out[1] = (unsigned char)((v >> 8) & 0xFFu);
    out[2] = (unsigned char)((v >> 16) & 0xFFu);
    out[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put_u64_le(unsigned char* out, unsigned long long v){
    for (unsigned int i = 0; i < 8u; i++){
        out[i] = (unsigned char)((v >> (8u * i)) & 0xFFu);
    }
}

static int alg_mask_has(unsigned int mask, unsigned int alg){
    if (alg >= 32u){
        return 0;
    }
    return (mask & (1u << alg)) != 0u;
}

static int auth_key_allowed(const trust_key_t* key){
    if (!key || key->revoked){
        return 0;
    }
    if ((key->scope_mask & TRUST_SCOPE_AUTH) == 0u){
        return 0;
    }
    if ((key->role_mask & (TRUST_ROLE_ADMIN | TRUST_ROLE_DEVELOPER)) == 0u){
        return 0;
    }
    if (!alg_mask_has(key->sig_alg_mask, QOS_SIG_ALG_ED25519)){
        return 0;
    }
    return 1;
}

static int auth_key_has_pq(const trust_key_t* key){
    if (!key){
        return 0;
    }
    if (key->pq_pubkey_len == 0u || key->pq_pubkey_len > QOS_PQ_MAX_PUBKEY_BYTES){
        return 0;
    }
    if (!alg_mask_has(key->pq_sig_alg_mask, QOS_SIG_ALG_MLDSA65)){
        return 0;
    }
    unsigned char nz = 0;
    for (unsigned int i = 0; i < key->pq_pubkey_len; i++){
        nz |= key->pq_pubkey[i];
    }
    return nz != 0u;
}

static int build_auth_sig_message(const unsigned char* auth_bytes,
                                  unsigned int auth_len,
                                  unsigned int signer_key_id,
                                  unsigned char* out,
                                  unsigned int out_cap,
                                  unsigned int* out_len){
    static const unsigned char tag[16] = {
        'Q','O','S','-','F','I','L','E','-','S','I','G','-','V','1','\0'
    };
    unsigned char digest[32];
    const unsigned int need = 16u + 4u + 4u + 4u + 8u + 1u + AUTH_SIG_LABEL_LEN + 32u;
    unsigned int o = 0u;
    if (!auth_bytes || auth_len == 0u || !out || !out_len || out_cap < need){
        return -1;
    }

    sha256_digest(auth_bytes, auth_len, digest);
    for (unsigned int i = 0; i < 16u; i++) out[o++] = tag[i];
    put_u32_le(out + o, 1u); o += 4u;
    put_u32_le(out + o, signer_key_id); o += 4u;
    put_u32_le(out + o, QOS_SIG_ALG_ED25519); o += 4u;
    put_u64_le(out + o, (unsigned long long)auth_len); o += 8u;
    out[o++] = (unsigned char)AUTH_SIG_LABEL_LEN;
    for (unsigned int i = 0; i < AUTH_SIG_LABEL_LEN; i++){
        out[o++] = (unsigned char)AUTH_SIG_LABEL[i];
    }
    for (unsigned int i = 0; i < 32u; i++){
        out[o++] = digest[i];
    }
    *out_len = o;
    return 0;
}

static int verify_auth_ed25519(const unsigned char* auth_bytes,
                               unsigned int auth_len,
                               const trust_key_t** out_key){
    static const unsigned int candidate_ids[] = {
        TRUST_KEY_ID_ADMIN_MAIN,
        TRUST_KEY_ID_DEV_MAIN
    };
    unsigned char msg[16u + 4u + 4u + 4u + 8u + 1u + AUTH_SIG_LABEL_LEN + 32u];
    unsigned int msg_len = 0u;

    if (!auth_bytes || auth_len == 0u || !out_key){
        return -1;
    }
    *out_key = 0;

    int sig_n = fat32_read_file(AUTH_SIG_FILE_83, g_auth_sig, (int)sizeof(g_auth_sig));
    if (sig_n != 64){
        uart_puts("AUTH: required AUTH.SIG missing or wrong size\n");
        return -1;
    }

    for (unsigned int i = 0; i < (unsigned int)(sizeof(candidate_ids) / sizeof(candidate_ids[0])); i++){
        const trust_key_t* key = trust_find_key(candidate_ids[i]);
        if (!auth_key_allowed(key)){
            continue;
        }
        if (build_auth_sig_message(auth_bytes, auth_len, key->key_id,
                                   msg, sizeof(msg), &msg_len) != 0){
            continue;
        }
        if (qos_ed25519_verify(g_auth_sig, msg, msg_len, key->ed25519_pubkey)){
            *out_key = key;
            uart_puts("AUTH: Ed25519 signature OK (");
            uart_puts(key->name);
            uart_puts(")\n");
            return 0;
        }
    }

    uart_puts("AUTH: Ed25519 signature verify failed\n");
    return -1;
}

static int verify_auth_pq(const unsigned char* auth_bytes,
                          unsigned int auth_len,
                          const trust_key_t* key){
    unsigned char msg[16u + 4u + 4u + 4u + 8u + 1u + AUTH_SIG_LABEL_LEN + 32u];
    unsigned char msg_digest[32];
    unsigned int msg_len = 0u;

    if (!auth_bytes || auth_len == 0u || !key){
        return -1;
    }
    if (!auth_key_has_pq(key)){
        uart_puts("AUTH: PQ required but signer lacks PQ material\n");
        return -1;
    }

    int n = fat32_read_file(AUTH_PQ_FILE_83, g_auth_pq_sig, (int)sizeof(g_auth_pq_sig));
    if (n <= 0){
        uart_puts("AUTH: required AUTH.PQS missing\n");
        return -1;
    }
    if (n < (int)QOS_PQ_SIG_HEADER_BYTES){
        uart_puts("AUTH: AUTH.PQS too small\n");
        return -1;
    }

    unsigned int magic = read_le32(&g_auth_pq_sig[0]);
    unsigned int version = read_le32(&g_auth_pq_sig[4]);
    unsigned int signer_key_id = read_le32(&g_auth_pq_sig[8]);
    unsigned int sig_alg = read_le32(&g_auth_pq_sig[12]);
    unsigned int sig_len = read_le32(&g_auth_pq_sig[16]);
    if (magic != QOS_PQ_SIG_MAGIC || version != QOS_PQ_SIG_VERSION){
        uart_puts("AUTH: invalid AUTH.PQS header\n");
        return -1;
    }
    if (signer_key_id != key->key_id){
        uart_puts("AUTH: AUTH.PQS signer mismatch\n");
        return -1;
    }
    if (!alg_mask_has(key->pq_sig_alg_mask, sig_alg)){
        uart_puts("AUTH: signer disallows AUTH.PQS algorithm\n");
        return -1;
    }
    if ((QOS_PQ_SIG_HEADER_BYTES + sig_len) > (unsigned int)n){
        uart_puts("AUTH: truncated AUTH.PQS signature\n");
        return -1;
    }
    if (build_auth_sig_message(auth_bytes, auth_len, key->key_id,
                               msg, sizeof(msg), &msg_len) != 0){
        return -1;
    }
    sha256_digest(msg, msg_len, msg_digest);
    if (pq_sig_verify_digest_sha256(sig_alg,
                                    msg_digest,
                                    &g_auth_pq_sig[QOS_PQ_SIG_HEADER_BYTES],
                                    sig_len,
                                    key->pq_pubkey,
                                    key->pq_pubkey_len) != 0){
        uart_puts("AUTH: PQ signature verify failed\n");
        return -1;
    }
    uart_puts("AUTH: PQ signature OK (");
    uart_puts(pq_sig_alg_name(sig_alg));
    uart_puts(")\n");
    return 0;
}

static int verify_auth_trust(const unsigned char* auth_bytes, unsigned int auth_len){
    const trust_key_t* key = 0;
    if (AUTH_REQUIRE_ED25519 && verify_auth_ed25519(auth_bytes, auth_len, &key) != 0){
        return -1;
    }
    if (AUTH_REQUIRE_PQ && verify_auth_pq(auth_bytes, auth_len, key) != 0){
        return -1;
    }
    return 0;
}

static void create_password_hash_sha256(const char* password,
                                        const unsigned char salt[AUTH_SALT_BYTES],
                                        unsigned char out_hash[AUTH_HASH_BYTES]){
    sha256_ctx_t ctx;
    unsigned int pw_len = cstr_len_bounded(password, 1024u);
    sha256_init(&ctx);
    if (password && pw_len){
        sha256_update(&ctx, (const unsigned char*)password, pw_len);
    }
    if (salt){
        sha256_update(&ctx, salt, AUTH_SALT_BYTES);
    }
    sha256_final(&ctx, out_hash);
}

void create_password_hash(const char* password,
                          const unsigned char salt[AUTH_SALT_BYTES],
                          unsigned char out_hash[AUTH_HASH_BYTES]){
    create_password_hash_sha256(password, salt, out_hash);
}

static int auth_argon2_params_valid(unsigned int t_cost,
                                    unsigned int m_cost_kib,
                                    unsigned int parallelism){
    if (t_cost == 0u || t_cost > AUTH_ARGON2_MAX_T_COST){
        return 0;
    }
    if (m_cost_kib < 8u || m_cost_kib > AUTH_ARGON2_MAX_M_COST_KIB){
        return 0;
    }
    if (parallelism == 0u || parallelism > AUTH_ARGON2_MAX_PARALLELISM){
        return 0;
    }
    return 1;
}

static int create_password_hash_kdf(const char* password,
                                    const unsigned char salt[AUTH_SALT_BYTES],
                                    unsigned char out_hash[AUTH_HASH_BYTES]){
    if (!out_hash || !salt){
        return -1;
    }

    if (g_auth.kdf_id == AUTH_KDF_SHA256){
        create_password_hash_sha256(password, salt, out_hash);
        return 0;
    }

    if (g_auth.kdf_id == AUTH_KDF_ARGON2ID){
        unsigned int pw_len = cstr_len_bounded(password, 1024u);
        int rc = argon2id_hash_raw_qos(g_auth.argon2_t_cost,
                                       g_auth.argon2_m_cost_kib,
                                       g_auth.argon2_parallelism,
                                       password ? (const void*)password : (const void*)"",
                                       pw_len,
                                       salt,
                                       AUTH_SALT_BYTES,
                                       out_hash,
                                       AUTH_HASH_BYTES,
                                       g_auth.argon2_version);
        return rc == 0 ? 0 : -1;
    }

    return -1;
}

static void compute_expected_response(const unsigned char client_nonce[AUTH_NONCE_BYTES],
                                      const unsigned char server_nonce[AUTH_NONCE_BYTES],
                                      unsigned char out_hash[AUTH_HASH_BYTES]){
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, g_auth.stored_hash, AUTH_HASH_BYTES);
    sha256_update(&ctx, client_nonce, AUTH_NONCE_BYTES);
    sha256_update(&ctx, server_nonce, AUTH_NONCE_BYTES);
    sha256_final(&ctx, out_hash);
}

static int parse_auth_v1(const unsigned char* buf, int n){
    unsigned int off = AUTH_MAGIC_BYTES;

    if (n < (int)AUTH_V1_FILE_MIN_BYTES){
        uart_puts("AUTH: file missing or too small\n");
        return -1;
    }
    if (buf[off++] != AUTH_VERSION_V1){
        uart_puts("AUTH: bad version\n");
        return -1;
    }

    g_auth.file_version = AUTH_VERSION_V1;
    g_auth.kdf_id = AUTH_KDF_SHA256;
    g_auth.argon2_t_cost = AUTH_ARGON2_DEFAULT_T_COST;
    g_auth.argon2_m_cost_kib = AUTH_ARGON2_DEFAULT_M_COST_KIB;
    g_auth.argon2_parallelism = AUTH_ARGON2_DEFAULT_PARALLELISM;
    g_auth.argon2_version = AUTH_ARGON2_DEFAULT_VERSION;

    g_auth.username_len = (unsigned int)buf[off++];
    if (g_auth.username_len == 0u || g_auth.username_len > AUTH_USERNAME_MAX){
        uart_puts("AUTH: bad username length\n");
        return -1;
    }

    for (unsigned int i = 0; i < AUTH_USERNAME_MAX + 1u; i++){
        unsigned char ch = buf[off + i];
        g_auth.username[i] = (i < g_auth.username_len) ? (char)ch : 0;
    }
    g_auth.username[g_auth.username_len] = 0;
    off += (AUTH_USERNAME_MAX + 1u);

    for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
        g_auth.salt[i] = buf[off + i];
    }
    off += AUTH_SALT_BYTES;

    for (unsigned int i = 0; i < AUTH_HASH_BYTES; i++){
        g_auth.stored_hash[i] = buf[off + i];
    }
    return 0;
}

static int parse_auth_v2(const unsigned char* buf, int n){
    unsigned int off = AUTH_MAGIC_BYTES;
    unsigned int t_cost;
    unsigned int m_cost_kib;
    unsigned int parallelism;
    unsigned int version;

    if (n < (int)AUTH_V2_FILE_MIN_BYTES){
        uart_puts("AUTH: v2 file too small\n");
        return -1;
    }
    if (buf[off++] != AUTH_VERSION_V2){
        uart_puts("AUTH: bad v2 version\n");
        return -1;
    }

    g_auth.file_version = AUTH_VERSION_V2;
    g_auth.username_len = (unsigned int)buf[off++];
    if (g_auth.username_len == 0u || g_auth.username_len > AUTH_USERNAME_MAX){
        uart_puts("AUTH: bad username length\n");
        return -1;
    }

    for (unsigned int i = 0; i < AUTH_USERNAME_MAX + 1u; i++){
        unsigned char ch = buf[off + i];
        g_auth.username[i] = (i < g_auth.username_len) ? (char)ch : 0;
    }
    g_auth.username[g_auth.username_len] = 0;
    off += (AUTH_USERNAME_MAX + 1u);

    g_auth.kdf_id = buf[off++];
    off += 1u; /* reserved byte */

    t_cost = read_le32(&buf[off]);
    off += 4u;
    m_cost_kib = read_le32(&buf[off]);
    off += 4u;
    parallelism = read_le32(&buf[off]);
    off += 4u;
    version = read_le32(&buf[off]);
    off += 4u;

    if (g_auth.kdf_id == AUTH_KDF_SHA256){
        g_auth.argon2_t_cost = AUTH_ARGON2_DEFAULT_T_COST;
        g_auth.argon2_m_cost_kib = AUTH_ARGON2_DEFAULT_M_COST_KIB;
        g_auth.argon2_parallelism = AUTH_ARGON2_DEFAULT_PARALLELISM;
        g_auth.argon2_version = AUTH_ARGON2_DEFAULT_VERSION;
    } else if (g_auth.kdf_id == AUTH_KDF_ARGON2ID){
        if (!auth_argon2_params_valid(t_cost, m_cost_kib, parallelism)){
            uart_puts("AUTH: invalid argon2 params\n");
            return -1;
        }
        g_auth.argon2_t_cost = t_cost;
        g_auth.argon2_m_cost_kib = m_cost_kib;
        g_auth.argon2_parallelism = parallelism;
        g_auth.argon2_version = version ? version : AUTH_ARGON2_DEFAULT_VERSION;
    } else{
        uart_puts("AUTH: unsupported kdf\n");
        return -1;
    }

    for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
        g_auth.salt[i] = buf[off + i];
    }
    off += AUTH_SALT_BYTES;

    for (unsigned int i = 0; i < AUTH_HASH_BYTES; i++){
        g_auth.stored_hash[i] = buf[off + i];
    }
    return 0;
}

int auth_init(void){
    static unsigned char buf[AUTH_FILE_MAX_BYTES];
    int n;

    auth_zero_state();

    if (fat32_init() != 0){
        uart_puts("AUTH: FAT init failed\n");
        return -1;
    }

    n = fat32_read_file(AUTH_FILE_83, buf, sizeof(buf));
    if (n < (int)AUTH_V1_FILE_MIN_BYTES){
        uart_puts("AUTH: file missing or too small\n");
        return -1;
    }
    if (n >= (int)sizeof(buf)){
        uart_puts("AUTH: file too large\n");
        return -1;
    }

    if (verify_auth_trust(buf, (unsigned int)n) != 0){
        uart_puts("AUTH: trust verification failed\n");
        return -1;
    }

    if (bytes_eq(buf, k_auth_magic_v2, AUTH_MAGIC_BYTES)){
        if (parse_auth_v2(buf, n) != 0){
            return -1;
        }
    } else if (bytes_eq(buf, k_auth_magic_v1, AUTH_MAGIC_BYTES)){
        if (parse_auth_v1(buf, n) != 0){
            return -1;
        }
    } else{
        uart_puts("AUTH: bad magic\n");
        return -1;
    }

    g_auth.ready = 1;
    uart_puts("AUTH: loaded user ");
    uart_puts(g_auth.username);
    uart_puts(" (kdf=");
    if (g_auth.kdf_id == AUTH_KDF_ARGON2ID){
        uart_puts("argon2id");
    } else{
        uart_puts("sha256");
    }
    uart_puts(")");
    uart_puts("\n");
    return 0;
}

int auth_is_ready(void){
    return g_auth.ready;
}

const char* auth_username(void){
    if (!g_auth.ready){
        return "";
    }
    return g_auth.username;
}

int auth_get_salt(unsigned char out_salt[AUTH_SALT_BYTES]){
    if (!out_salt || !g_auth.ready){
        return -1;
    }
    for (unsigned int i = 0; i < AUTH_SALT_BYTES; i++){
        out_salt[i] = g_auth.salt[i];
    }
    return 0;
}

int auth_issue_nonce(unsigned char out_nonce[AUTH_NONCE_BYTES]){
    if (!out_nonce){
        return -1;
    }
    if (crypto_random_bytes(out_nonce, AUTH_NONCE_BYTES) != 0){
        return -1;
    }
    return 0;
}

int auth_get_kdf_info(auth_kdf_info_t* out_info){
    if (!out_info || !g_auth.ready){
        return -1;
    }
    out_info->kdf_id = g_auth.kdf_id;
    out_info->argon2_t_cost = g_auth.argon2_t_cost;
    out_info->argon2_m_cost_kib = g_auth.argon2_m_cost_kib;
    out_info->argon2_parallelism = g_auth.argon2_parallelism;
    out_info->argon2_version = g_auth.argon2_version;
    return 0;
}

int auth_verify_password(const char* username, const char* password){
    unsigned int name_len;
    unsigned char calc_hash[AUTH_HASH_BYTES];
    int ok;

    if (!g_auth.ready || !username || !password){
        return -1;
    }

    name_len = cstr_len_bounded(username, AUTH_USERNAME_MAX + 1u);
    if (name_len != g_auth.username_len){
        return -1;
    }
    for (unsigned int i = 0; i < name_len; i++){
        if (username[i] != g_auth.username[i]){
            return -1;
        }
    }

    if (create_password_hash_kdf(password, g_auth.salt, calc_hash) != 0){
        crypto_memzero(calc_hash, sizeof(calc_hash));
        return -1;
    }
    ok = crypto_consttime_equal(calc_hash, g_auth.stored_hash, AUTH_HASH_BYTES);
    crypto_memzero(calc_hash, sizeof(calc_hash));
    return ok ? 0 : -1;
}

int auth_verify_response(const char* username,
                         const unsigned char client_nonce[AUTH_NONCE_BYTES],
                         const unsigned char server_nonce[AUTH_NONCE_BYTES],
                         const unsigned char response[AUTH_HASH_BYTES]){
    unsigned char expected[AUTH_HASH_BYTES];
    unsigned int name_len;

    if (!g_auth.ready || !username || !client_nonce || !server_nonce || !response){
        return -1;
    }

    name_len = cstr_len_bounded(username, AUTH_USERNAME_MAX + 1u);
    if (name_len != g_auth.username_len){
        return -1;
    }
    for (unsigned int i = 0; i < name_len; i++){
        if (username[i] != g_auth.username[i]){
            return -1;
        }
    }

    compute_expected_response(client_nonce, server_nonce, expected);
    int ok = crypto_consttime_equal(expected, response, AUTH_HASH_BYTES);
    crypto_memzero(expected, sizeof(expected));
    return ok ? 0 : -1;
}
