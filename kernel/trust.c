#include "trust.h"
#include "sha256.h"
#include "uart.h"
#include "ed25519_verify.h"
#include "trust_keys_autogen.h"
#include "fat32.h"

#define TRUST_ART_USER_APP 0u
#define TRUST_ART_SHELL    1u
#define TRUST_ART_WEB      2u

#define TRUST_KEY_ADMIN_MAIN 0x00000001u
#define TRUST_KEY_DEV_MAIN   0x00010001u

#define TRUST_PQ_SIDECAR_MAX QOS_PQ_SIG_MAX

static const trust_key_t g_keys[] = {
    {
        TRUST_KEY_ADMIN_MAIN,
        "admin-main",
        TRUST_ROLE_ADMIN,
        TRUST_SCOPE_KERNEL | TRUST_SCOPE_SHELL | TRUST_SCOPE_WEB | TRUST_SCOPE_USER_APP,
        (1u << QOS_SIG_ALG_DIGEST_ONLY) | (1u << QOS_SIG_ALG_ED25519),
        (1u << QOS_SIG_ALG_MLDSA65),
        TRUST_ADMIN_ED25519_PUBKEY_INIT,
        TRUST_ADMIN_PQ_PUBKEY_LEN,
        TRUST_ADMIN_PQ_PUBKEY_INIT,
        0
    },
    {
        TRUST_KEY_DEV_MAIN,
        "dev-main",
        TRUST_ROLE_DEVELOPER,
        TRUST_SCOPE_USER_APP,
        (1u << QOS_SIG_ALG_DIGEST_ONLY) | (1u << QOS_SIG_ALG_ED25519),
        (1u << QOS_SIG_ALG_MLDSA65),
        TRUST_DEV_ED25519_PUBKEY_INIT,
        TRUST_DEV_PQ_PUBKEY_LEN,
        TRUST_DEV_PQ_PUBKEY_INIT,
        0
    }
};

static unsigned char g_pq_sidecar_buf[TRUST_PQ_SIDECAR_MAX];
static int g_warned_digest_only = 0;
static int g_warned_missing_program_pq = 0;
static const int g_require_ed25519 = 1;
static const int g_require_program_pq = 1;

static void put_u32_le(unsigned char* out, unsigned int v){
    out[0] = (unsigned char)(v & 0xFFu);
    out[1] = (unsigned char)((v >> 8) & 0xFFu);
    out[2] = (unsigned char)((v >> 16) & 0xFFu);
    out[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static unsigned int get_u32_le(const unsigned char* p){
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static int read_program_layout_v1(const program_sec_header_t* sec,
                                  unsigned int code_size,
                                  unsigned int* out_rw_off,
                                  unsigned int* out_rw_size){
    const unsigned int page_size = 4096u;
    const unsigned int sec_min = (unsigned int)sizeof(program_sec_header_t);
    const unsigned int need = sec_min + (unsigned int)sizeof(program_sec_layout_v1_t);
    if (!sec || !out_rw_off || !out_rw_size){
        return -1;
    }
    if ((sec->flags & QOS_PROG_FLAG_MEM_LAYOUT_V1) == 0u){
        return -1;
    }
    if (sec->header_size < need){
        return -1;
    }

    const unsigned char* p = (const unsigned char*)sec + sec_min;
    unsigned int rw_off = get_u32_le(&p[0]);
    unsigned int rw_size = get_u32_le(&p[4]);
    (void)code_size;
    if (rw_size == 0u){
        return -1;
    }
    if ((rw_off & (page_size - 1u)) != 0u){
        return -1;
    }
    *out_rw_off = rw_off;
    *out_rw_size = rw_size;
    return 0;
}

static int build_program_sig_message(const char* fat_name_83,
                                     const program_sec_header_t* sec,
                                     unsigned int code_size,
                                     unsigned char* out,
                                     unsigned int out_cap){
    static const unsigned char tag[16] = {
        'Q','O','S','-','P','R','O','G','-','S','I','G','-','V','1','\0'
    };
    unsigned int rw_off = 0;
    unsigned int rw_size = 0;
    const unsigned int need = 16u + 11u + 4u + 4u + 4u + 4u + 4u + 4u + 32u;
    if (!out || out_cap < need || !fat_name_83 || !sec){
        return -1;
    }
    if (read_program_layout_v1(sec, code_size, &rw_off, &rw_size) != 0){
        return -1;
    }

    unsigned int o = 0;
    for (unsigned int i = 0; i < 16u; i++) out[o++] = tag[i];
    for (unsigned int i = 0; i < 11u; i++) out[o++] = (unsigned char)fat_name_83[i];
    put_u32_le(out + o, sec->flags); o += 4u;
    put_u32_le(out + o, sec->signer_key_id); o += 4u;
    put_u32_le(out + o, sec->sig_alg); o += 4u;
    put_u32_le(out + o, code_size); o += 4u;
    put_u32_le(out + o, rw_off); o += 4u;
    put_u32_le(out + o, rw_size); o += 4u;
    for (unsigned int i = 0; i < 32u; i++) out[o++] = sec->sha256[i];
    return (int)o;
}

static int fat_name_eq(const char* a, const char* b){
    if (!a || !b){
        return 0;
    }
    for (int i = 0; i < 11; i++){
        if (a[i] != b[i]){
            return 0;
        }
    }
    return 1;
}

static unsigned int classify_artifact(const char* fat_name_83){
    if (fat_name_eq(fat_name_83, "SHELL   BIN")){
        return TRUST_ART_SHELL;
    }
    if (fat_name_eq(fat_name_83, "WEBBROWSBIN")){
        return TRUST_ART_WEB;
    }
    return TRUST_ART_USER_APP;
}

const trust_key_t* trust_find_key(unsigned int key_id){
    for (unsigned int i = 0; i < (unsigned int)(sizeof(g_keys) / sizeof(g_keys[0])); i++){
        if (g_keys[i].key_id == key_id){
            return &g_keys[i];
        }
    }
    return 0;
}

static int has_scope(unsigned int scope_mask, unsigned int scope){
    return (scope_mask & scope) == scope;
}

static int alg_mask_has(unsigned int mask, unsigned int alg){
    if (alg >= 32u){
        return 0;
    }
    return (mask & (1u << alg)) != 0u;
}

static int key_has_pq_pubkey(const trust_key_t* key){
    if (!key){
        return 0;
    }
    if (key->pq_pubkey_len == 0u || key->pq_pubkey_len > QOS_PQ_MAX_PUBKEY_BYTES){
        return 0;
    }
    unsigned char nz = 0;
    for (unsigned int i = 0; i < key->pq_pubkey_len; i++){
        nz |= key->pq_pubkey[i];
    }
    return nz != 0u;
}

static void build_pq_sidecar_name(const char* fat_name_83, char out_name_83[12]){
    for (unsigned int i = 0; i < 8u; i++){
        out_name_83[i] = fat_name_83[i];
    }
    out_name_83[8] = 'P';
    out_name_83[9] = 'Q';
    out_name_83[10] = 'S';
    out_name_83[11] = 0;
}

static int verify_program_pq_sidecar(const char* fat_name_83,
                                     const program_sec_header_t* sec,
                                     unsigned int code_size,
                                     const trust_key_t* key,
                                     int required){
    unsigned char msg[16u + 11u + 4u + 4u + 4u + 4u + 4u + 4u + 32u];
    unsigned char msg_digest[32];
    char pq_name[12];

    if (!fat_name_83 || !sec || !key){
        return -1;
    }

    if (!key_has_pq_pubkey(key) ||
        !alg_mask_has(key->pq_sig_alg_mask, QOS_SIG_ALG_MLDSA65)){
        if (required){
            uart_puts("Trust: PQ signature required but signer has no PQ key: ");
            uart_puts(key->name);
            uart_puts("\n");
            return -1;
        }
        uart_puts("Trust: signer has no PQ key; PQ skipped for ");
        uart_puts(key->name);
        uart_puts("\n");
        return 0;
    }

    build_pq_sidecar_name(fat_name_83, pq_name);
    int n = fat32_read_file(pq_name, g_pq_sidecar_buf, (int)sizeof(g_pq_sidecar_buf));
    if (n <= 0){
        if (required){
            uart_puts("Trust: required PQ sidecar missing for ");
            uart_puts(key->name);
            uart_puts(".\n");
            return -1;
        }
        if (!g_warned_missing_program_pq){
            uart_puts("Trust: PQ sidecar not present; continuing with Ed25519 only.\n");
            g_warned_missing_program_pq = 1;
        }
        return 0;
    }

    if (n < (int)QOS_PQ_SIG_HEADER_BYTES){
        uart_puts("Trust: PQ sidecar too small.\n");
        return -1;
    }

    unsigned int magic = get_u32_le(&g_pq_sidecar_buf[0]);
    unsigned int version = get_u32_le(&g_pq_sidecar_buf[4]);
    unsigned int signer_key_id = get_u32_le(&g_pq_sidecar_buf[8]);
    unsigned int sig_alg = get_u32_le(&g_pq_sidecar_buf[12]);
    unsigned int sig_len = get_u32_le(&g_pq_sidecar_buf[16]);

    if (magic != QOS_PQ_SIG_MAGIC || version != QOS_PQ_SIG_VERSION){
        uart_puts("Trust: invalid PQ sidecar header.\n");
        return -1;
    }
    if (signer_key_id != sec->signer_key_id){
        uart_puts("Trust: PQ sidecar signer mismatch.\n");
        return -1;
    }
    if (!alg_mask_has(key->pq_sig_alg_mask, sig_alg)){
        uart_puts("Trust: signer disallows PQ signature algorithm.\n");
        return -1;
    }
    if ((QOS_PQ_SIG_HEADER_BYTES + sig_len) > (unsigned int)n){
        uart_puts("Trust: truncated PQ sidecar signature.\n");
        return -1;
    }

    int msg_len = build_program_sig_message(fat_name_83, sec, code_size, msg, sizeof(msg));
    if (msg_len <= 0){
        uart_puts("Trust: failed to build PQ signature payload.\n");
        return -1;
    }
    sha256_digest(msg, (unsigned int)msg_len, msg_digest);

    if (pq_sig_verify_digest_sha256(sig_alg,
                                    msg_digest,
                                    &g_pq_sidecar_buf[QOS_PQ_SIG_HEADER_BYTES], sig_len,
                                    key->pq_pubkey, key->pq_pubkey_len) != 0){
        uart_puts("Trust: PQ signature verify failed.\n");
        return -1;
    }
    uart_puts("Trust: PQ signature OK for ");
    uart_puts(key->name);
    uart_puts(" (");
    uart_puts(pq_sig_alg_name(sig_alg));
    uart_puts(").\n");
    return 0;
}

int trust_verify_program_image(const char* fat_name_83,
                               const program_sec_header_t* sec,
                               const unsigned char* code,
                               unsigned int code_size){
    unsigned int rw_off = 0;
    unsigned int rw_size = 0;
    if (!sec){
        uart_puts("Trust: missing security header.\n");
        return -1;
    }
    if (sec->magic != QOS_SEC_MAGIC){
        uart_puts("Trust: bad security header magic.\n");
        return -1;
    }
    if (sec->header_size < sizeof(program_sec_header_t)){
        uart_puts("Trust: bad security header size.\n");
        return -1;
    }
    if (sec->sig_alg != QOS_SIG_ALG_DIGEST_ONLY && sec->sig_alg != QOS_SIG_ALG_ED25519){
        uart_puts("Trust: unsupported signature algorithm.\n");
        return -1;
    }
    if (g_require_ed25519 && sec->sig_alg != QOS_SIG_ALG_ED25519){
        uart_puts("Trust: signature policy requires Ed25519.\n");
        return -1;
    }
    if ((sec->flags & QOS_PROG_FLAG_SHA256) == 0u){
        uart_puts("Trust: SHA-256 flag required.\n");
        return -1;
    }
    if ((sec->flags & QOS_PROG_FLAG_MEM_LAYOUT_V1) == 0u){
        uart_puts("Trust: MEM_LAYOUT_V1 flag required.\n");
        return -1;
    }
    if (read_program_layout_v1(sec, code_size, &rw_off, &rw_size) != 0){
        uart_puts("Trust: invalid MEM_LAYOUT_V1.\n");
        return -1;
    }

    const trust_key_t* key = trust_find_key(sec->signer_key_id);
    if (!key){
        uart_puts("Trust: signer key not trusted.\n");
        return -1;
    }
    if (key->revoked){
        uart_puts("Trust: signer key revoked.\n");
        return -1;
    }
    if (!alg_mask_has(key->sig_alg_mask, sec->sig_alg)){
        uart_puts("Trust: signer key does not allow this signature algorithm.\n");
        return -1;
    }

    unsigned int artifact = classify_artifact(fat_name_83);
    unsigned int req_scope = TRUST_SCOPE_USER_APP;
    unsigned int req_role = TRUST_ROLE_DEVELOPER | TRUST_ROLE_ADMIN;
    if (artifact == TRUST_ART_SHELL){
        req_scope = TRUST_SCOPE_SHELL;
        req_role = TRUST_ROLE_ADMIN;
    } else if (artifact == TRUST_ART_WEB){
        req_scope = TRUST_SCOPE_WEB;
        req_role = TRUST_ROLE_ADMIN;
    }

    if (!has_scope(key->scope_mask, req_scope)){
        uart_puts("Trust: signer key missing required scope.\n");
        return -1;
    }
    if ((key->role_mask & req_role) == 0u){
        uart_puts("Trust: signer key missing required role.\n");
        return -1;
    }

    unsigned char digest[32];
    sha256_digest(code, code_size, digest);
    for (unsigned int i = 0; i < sizeof(digest); i++){
        if (digest[i] != sec->sha256[i]){
            uart_puts("Trust: program digest mismatch.\n");
            return -1;
        }
    }

    if (sec->sig_alg == QOS_SIG_ALG_DIGEST_ONLY){
        if (sec->sig_len != 0u){
            uart_puts("Trust: digest-only signature must have sig_len=0.\n");
            return -1;
        }
        if (!g_warned_digest_only){
            uart_puts("Trust: digest-only signatures enabled (development mode).\n");
            g_warned_digest_only = 1;
        }
        return 0;
    }

    if (sec->sig_alg == QOS_SIG_ALG_ED25519){
        if (sec->sig_len != 64u){
            uart_puts("Trust: Ed25519 signature must be 64 bytes.\n");
            return -1;
        }
        unsigned char msg[16u + 11u + 4u + 4u + 4u + 4u + 4u + 4u + 32u];
        int msg_len = build_program_sig_message(fat_name_83, sec, code_size, msg, sizeof(msg));
        if (msg_len <= 0){
            uart_puts("Trust: failed to build signature payload.\n");
            return -1;
        }
        if (!qos_ed25519_verify(sec->signature, msg, (unsigned int)msg_len, key->ed25519_pubkey)){
            uart_puts("Trust: Ed25519 signature verify failed.\n");
            return -1;
        }

        uart_puts("Trust: Ed25519 signature OK for ");
        uart_puts(key->name);
        uart_puts(".\n");
        if (verify_program_pq_sidecar(fat_name_83, sec, code_size, key, g_require_program_pq) != 0){
            return -1;
        }
        return 0;
    }

    return 0;
}
