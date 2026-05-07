#include "kernel_verify.h"
#include "program.h"
#include "sha256.h"
#include "trust.h"
#include "uart.h"
#include "kernel_manifest_autogen.h"
#include "fat32.h"
#include "ed25519_verify.h"

extern unsigned char __kernel_text_start[];
extern unsigned char __kernel_rodata_verify_end[];
extern unsigned char __kernel_manifest_start[];
extern unsigned char __kernel_manifest_end[];

typedef struct {
    unsigned int manifest_version;
    unsigned int signer_key_id;
    unsigned int sig_alg;
    unsigned int flags;
    unsigned int sig_len;
    unsigned char digest[32];      // digest over in-memory verify span
    unsigned char file_digest[32]; // digest over kernel8.img excluding .kmanifest bytes
    unsigned char signature[QOS_MAX_SIGNATURE_BYTES];
} kernel_manifest_t;

// Framework mode:
// 0 = warn only, continue boot on failure.
// 1 = halt boot on failure.
static const int g_kernel_verify_enforce = 0;
static const int g_require_kernel_ed25519 = 1;
static const int g_require_kernel_pq = 0;

static int g_warned_kernel_digest_only = 0;
static int g_logged_kernel_verify_mode = 0;
static int g_logged_kernel_ed25519_ok = 0;
static int g_warned_kernel_pq_missing = 0;

// Replace digest + signer metadata during provisioning.
static volatile const kernel_manifest_t g_kernel_manifest
__attribute__((section(".kmanifest"), used)) = {
    KERNEL_MANIFEST_VERSION,
    KERNEL_MANIFEST_SIGNER_KEY_ID,
    KERNEL_MANIFEST_SIG_ALG,
    KERNEL_MANIFEST_FLAGS,
    KERNEL_MANIFEST_SIG_LEN,
    KERNEL_MANIFEST_DIGEST_INIT,
    KERNEL_MANIFEST_FILE_DIGEST_INIT,
    KERNEL_MANIFEST_SIGNATURE_INIT
};

#define KERNEL_IMAGE_FAT_NAME "KERNEL8 IMG"
#define KERNEL_PQ_SIG_FAT_NAME "KERNEL8 PQS"
#define KERNEL_IMAGE_MAX_SIZE (4u * 1024u * 1024u)
#define KERNEL_PQ_SIG_HEADER_BYTES QOS_PQ_SIG_HEADER_BYTES
#define KERNEL_PQ_SIG_MAX QOS_PQ_SIG_MAX

static unsigned char g_kernel_file_buf[KERNEL_IMAGE_MAX_SIZE];
static unsigned char g_kernel_pq_sig_buf[KERNEL_PQ_SIG_MAX];
static int g_kernel_pq_cached = 0;
static int g_kernel_pq_cached_rc = 0;

static char nibble_hex(unsigned int v){
    return (v < 10u) ? (char)('0' + v) : (char)('A' + (v - 10u));
}

static void uart_puthex_byte(unsigned char b){
    uart_send(nibble_hex((b >> 4) & 0xFu));
    uart_send(nibble_hex(b & 0xFu));
}

static void uart_put_digest(const unsigned char d[32]){
    for (unsigned int i = 0; i < 32u; i++){
        uart_puthex_byte(d[i]);
    }
    uart_puts("\n");
}

static int digest_is_all_zero(const unsigned char d[32]){
    for (unsigned int i = 0; i < 32u; i++){
        if (d[i] != 0u){
            return 0;
        }
    }
    return 1;
}

static int digest_equal(const unsigned char a[32], const unsigned char b[32]){
    for (unsigned int i = 0; i < 32u; i++){
        if (a[i] != b[i]){
            return 0;
        }
    }
    return 1;
}

static int alg_mask_has(unsigned int mask, unsigned int alg){
    if (alg >= 32u){
        return 0;
    }
    return (mask & (1u << alg)) != 0u;
}

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

static int build_kernel_sig_message(unsigned char* out, unsigned int out_cap, unsigned int* out_len){
    static const unsigned char tag[16] = {
        'Q','O','S','-','K','E','R','N','-','S','I','G','-','V','1','\0'
    };
    const unsigned int need = 16u + 4u + 4u + 4u + 4u + 32u + 32u;
    if (!out || !out_len || out_cap < need){
        return -1;
    }
    unsigned int o = 0;
    for (unsigned int i = 0; i < 16u; i++) out[o++] = tag[i];
    put_u32_le(out + o, g_kernel_manifest.manifest_version); o += 4u;
    put_u32_le(out + o, g_kernel_manifest.signer_key_id); o += 4u;
    put_u32_le(out + o, g_kernel_manifest.sig_alg); o += 4u;
    put_u32_le(out + o, g_kernel_manifest.flags); o += 4u;
    for (unsigned int i = 0; i < 32u; i++) out[o++] = g_kernel_manifest.digest[i];
    for (unsigned int i = 0; i < 32u; i++) out[o++] = g_kernel_manifest.file_digest[i];
    *out_len = o;
    return 0;
}

static int verify_manifest_policy(void){
    unsigned int signer_key_id = g_kernel_manifest.signer_key_id;
    unsigned int sig_alg = g_kernel_manifest.sig_alg;
    unsigned int flags = g_kernel_manifest.flags;
    unsigned int sig_len = g_kernel_manifest.sig_len;

    const trust_key_t* key = trust_find_key(signer_key_id);
    if (!key){
        uart_puts("Kernel verify: signer key not trusted.\n");
        return -1;
    }
    if (key->revoked){
        uart_puts("Kernel verify: signer key revoked.\n");
        return -1;
    }
    if ((key->role_mask & TRUST_ROLE_ADMIN) == 0u){
        uart_puts("Kernel verify: signer key missing admin role.\n");
        return -1;
    }
    if ((key->scope_mask & TRUST_SCOPE_KERNEL) == 0u){
        uart_puts("Kernel verify: signer key missing kernel scope.\n");
        return -1;
    }
    if (!alg_mask_has(key->sig_alg_mask, sig_alg)){
        uart_puts("Kernel verify: signer key disallows signature algorithm.\n");
        return -1;
    }
    if (sig_alg != QOS_SIG_ALG_DIGEST_ONLY &&
        sig_alg != QOS_SIG_ALG_ED25519){
        uart_puts("Kernel verify: unsupported signature algorithm.\n");
        return -1;
    }
    if (g_require_kernel_ed25519 && sig_alg != QOS_SIG_ALG_ED25519){
        uart_puts("Kernel verify: signature policy requires Ed25519.\n");
        return -1;
    }
    if ((flags & QOS_PROG_FLAG_SHA256) == 0u){
        uart_puts("Kernel verify: SHA-256 flag missing.\n");
        return -1;
    }
    if (sig_alg == QOS_SIG_ALG_DIGEST_ONLY){
        if (sig_len != 0u){
            uart_puts("Kernel verify: digest-only expects sig_len=0.\n");
            return -1;
        }
        if (!g_logged_kernel_verify_mode){
            uart_puts("Kernel verify: mode=DIGEST_ONLY\n");
            g_logged_kernel_verify_mode = 1;
        }
    } else if (sig_alg == QOS_SIG_ALG_ED25519){
        if (sig_len != 64u){
            uart_puts("Kernel verify: Ed25519 expects sig_len=64.\n");
            return -1;
        }
        if (!g_logged_kernel_verify_mode){
            uart_puts("Kernel verify: mode=ED25519\n");
            g_logged_kernel_verify_mode = 1;
        }
    }
    return 0;
}

static int verify_manifest_signature(const trust_key_t* key){
    if (!key){
        return -1;
    }
    unsigned int sig_alg = g_kernel_manifest.sig_alg;
    if (sig_alg == QOS_SIG_ALG_DIGEST_ONLY){
        if (!g_warned_kernel_digest_only){
            uart_puts("Kernel verify: digest-only mode enabled (development mode).\n");
            g_warned_kernel_digest_only = 1;
        }
        return 0;
    }

    if (sig_alg == QOS_SIG_ALG_ED25519){
        unsigned char msg[16u + 4u + 4u + 4u + 4u + 32u + 32u];
        unsigned char sig_copy[64];
        unsigned int msg_len = 0;
        if (build_kernel_sig_message(msg, sizeof(msg), &msg_len) != 0){
            return -1;
        }
        for (unsigned int i = 0; i < 64u; i++){
            sig_copy[i] = g_kernel_manifest.signature[i];
        }
        if (!qos_ed25519_verify(sig_copy, msg, msg_len, key->ed25519_pubkey)){
            uart_puts("Kernel verify: Ed25519 manifest signature failed.\n");
            return -1;
        }
        if (!g_logged_kernel_ed25519_ok){
            uart_puts("Kernel verify: Ed25519 manifest signature OK.\n");
            g_logged_kernel_ed25519_ok = 1;
        }
        return 0;
    }

    return -1;
}

static int verify_manifest_pq_sidecar_uncached(const trust_key_t* key){
    if (!key){
        return -1;
    }
    if (!key_has_pq_pubkey(key) ||
        !alg_mask_has(key->pq_sig_alg_mask, QOS_SIG_ALG_MLDSA65)){
        if (g_require_kernel_pq){
            uart_puts("Kernel verify: PQ required but trusted key lacks PQ material.\n");
            return -1;
        }
        return 0;
    }

    if (fat32_init() != 0){
        if (g_require_kernel_pq){
            uart_puts("Kernel verify: PQ FAT init failed.\n");
            return -1;
        }
        return 0;
    }

    int n = fat32_read_file(KERNEL_PQ_SIG_FAT_NAME, g_kernel_pq_sig_buf, (int)sizeof(g_kernel_pq_sig_buf));
    if (n <= 0){
        if (g_require_kernel_pq){
            uart_puts("Kernel verify: required PQ sidecar missing.\n");
            return -1;
        }
        if (!g_warned_kernel_pq_missing){
            uart_puts("Kernel verify: PQ sidecar missing; continuing with Ed25519 only.\n");
            g_warned_kernel_pq_missing = 1;
        }
        return 0;
    }
    if (n < (int)KERNEL_PQ_SIG_HEADER_BYTES){
        uart_puts("Kernel verify: PQ sidecar too small.\n");
        return -1;
    }

    unsigned int magic = get_u32_le(&g_kernel_pq_sig_buf[0]);
    unsigned int version = get_u32_le(&g_kernel_pq_sig_buf[4]);
    unsigned int signer_key_id = get_u32_le(&g_kernel_pq_sig_buf[8]);
    unsigned int sig_alg = get_u32_le(&g_kernel_pq_sig_buf[12]);
    unsigned int sig_len = get_u32_le(&g_kernel_pq_sig_buf[16]);

    if (magic != QOS_PQ_SIG_MAGIC || version != QOS_PQ_SIG_VERSION){
        uart_puts("Kernel verify: invalid PQ sidecar header.\n");
        return -1;
    }
    if (signer_key_id != g_kernel_manifest.signer_key_id){
        uart_puts("Kernel verify: PQ signer mismatch.\n");
        return -1;
    }
    if (!alg_mask_has(key->pq_sig_alg_mask, sig_alg)){
        uart_puts("Kernel verify: signer disallows PQ algorithm.\n");
        return -1;
    }
    if ((KERNEL_PQ_SIG_HEADER_BYTES + sig_len) > (unsigned int)n){
        uart_puts("Kernel verify: truncated PQ signature.\n");
        return -1;
    }

    unsigned char msg[16u + 4u + 4u + 4u + 4u + 32u + 32u];
    unsigned int msg_len = 0;
    unsigned char msg_digest[32];
    if (build_kernel_sig_message(msg, sizeof(msg), &msg_len) != 0){
        return -1;
    }
    sha256_digest(msg, msg_len, msg_digest);

    if (pq_sig_verify_digest_sha256(sig_alg,
                                    msg_digest,
                                    &g_kernel_pq_sig_buf[KERNEL_PQ_SIG_HEADER_BYTES], sig_len,
                                    key->pq_pubkey, key->pq_pubkey_len) != 0){
        uart_puts("Kernel verify: PQ signature failed.\n");
        return -1;
    }
    uart_puts("Kernel verify: PQ signature OK (");
    uart_puts(pq_sig_alg_name(sig_alg));
    uart_puts(").\n");
    return 0;
}

static int verify_manifest_pq_sidecar(const trust_key_t* key){
    if (g_kernel_pq_cached){
        return g_kernel_pq_cached_rc;
    }
    g_kernel_pq_cached_rc = verify_manifest_pq_sidecar_uncached(key);
    g_kernel_pq_cached = 1;
    return g_kernel_pq_cached_rc;
}

int kernel_verify_self(void){
    uart_puts("Kernel verify: start\n");
    if (verify_manifest_policy() != 0){
        return -1;
    }
    const trust_key_t* key = trust_find_key(g_kernel_manifest.signer_key_id);
    if (verify_manifest_signature(key) != 0){
        return -1;
    }
    if (verify_manifest_pq_sidecar(key) != 0){
        return -1;
    }

    unsigned char digest[32];
    unsigned long start = (unsigned long)__kernel_text_start;
    unsigned long end = (unsigned long)__kernel_rodata_verify_end;
    if (end <= start){
        uart_puts("Kernel verify: invalid memory bounds.\n");
        uart_puts(" start=");
        uart_puthex((unsigned int)start);
        uart_puts(" end=");
        uart_puthex((unsigned int)end);
        uart_puts("\n");
        return -1;
    }
    unsigned long image_len_ul = end - start;
    if (image_len_ul > (16ul * 1024ul * 1024ul)){
        uart_puts("Kernel verify: memory span too large.\n");
        uart_puts(" len=");
        uart_puthex((unsigned int)image_len_ul);
        uart_puts("\n");
        return -1;
    }
    sha256_digest((const unsigned char*)start, (unsigned int)image_len_ul, digest);

    unsigned char expected_digest[32];
    for (unsigned int i = 0; i < 32u; i++){
        expected_digest[i] = g_kernel_manifest.digest[i];
    }

    if (digest_is_all_zero(expected_digest)){
        uart_puts("Kernel verify: manifest digest not provisioned.\n");
        uart_puts("Kernel verify: measured digest=");
        uart_put_digest(digest);
        return 1;
    }

    if (!digest_equal(digest, expected_digest)){
        uart_puts("Kernel verify: FAILED (digest mismatch).\n");
        uart_puts("Kernel verify: measured=");
        uart_put_digest(digest);
        uart_puts("Kernel verify: expected=");
        uart_put_digest(expected_digest);
        return -1;
    }

    uart_puts("Kernel verify: measured=");
    uart_put_digest(digest);
    uart_puts("Kernel verify: OK.\n");
    return 0;
}

int kernel_verify_enforce(void){
    return g_kernel_verify_enforce;
}

int kernel_verify_storage_image(void){
    if (verify_manifest_policy() != 0){
        return -1;
    }
    const trust_key_t* key = trust_find_key(g_kernel_manifest.signer_key_id);
    if (verify_manifest_signature(key) != 0){
        return -1;
    }
    if (verify_manifest_pq_sidecar(key) != 0){
        return -1;
    }

    if (fat32_init() != 0){
        uart_puts("Kernel file verify: FAT init failed.\n");
        return -1;
    }

    int n = fat32_read_file(KERNEL_IMAGE_FAT_NAME, g_kernel_file_buf, (int)KERNEL_IMAGE_MAX_SIZE);
    if (n <= 0){
        uart_puts("Kernel file verify: read kernel8.img failed.\n");
        return -1;
    }

    const unsigned long load_base = 0x80000UL;
    unsigned long man_start = (unsigned long)__kernel_manifest_start;
    unsigned long man_end = (unsigned long)__kernel_manifest_end;
    if (man_end <= man_start || man_start < load_base){
        uart_puts("Kernel file verify: bad manifest section bounds.\n");
        return -1;
    }

    unsigned long off0 = man_start - load_base;
    unsigned long off1 = man_end - load_base;
    if (off1 > (unsigned long)n || off0 >= off1){
        uart_puts("Kernel file verify: manifest section not inside file span.\n");
        return -1;
    }

    unsigned char digest[32];
    unsigned int left_len = (unsigned int)off0;
    unsigned int right_len = (unsigned int)((unsigned long)n - off1);

    // Canonicalized full-image digest: bytes before manifest section + bytes after.
    // (Excludes embedded manifest to avoid self-reference recursion.)
    sha256_digest_concat2(g_kernel_file_buf, left_len,
                          g_kernel_file_buf + off1, right_len,
                          digest);

    unsigned char expected_file_digest[32];
    for (unsigned int i = 0; i < 32u; i++){
        expected_file_digest[i] = g_kernel_manifest.file_digest[i];
    }

    if (digest_is_all_zero(expected_file_digest)){
        uart_puts("Kernel file verify: manifest file digest not provisioned.\n");
        uart_puts("Kernel file verify: measured=");
        uart_put_digest(digest);
        return 1;
    }

    if (!digest_equal(digest, expected_file_digest)){
        uart_puts("Kernel file verify: FAILED (digest mismatch).\n");
        uart_puts("Kernel file verify: measured=");
        uart_put_digest(digest);
        uart_puts("Kernel file verify: expected=");
        uart_put_digest(expected_file_digest);
        return -1;
    }

    uart_puts("Kernel file verify: measured=");
    uart_put_digest(digest);
    uart_puts("Kernel file verify: OK.\n");
    return 0;
}
