#include "kernel_verify.h"
#include "program.h"
#include "sha256.h"
#include "trust.h"
#include "uart.h"
#include "kernel_manifest_autogen.h"

extern unsigned char __kernel_text_start[];
extern unsigned char __kernel_text_end[];
extern unsigned char __kernel_rodata_start[];
extern unsigned char __kernel_rodata_end[];

typedef struct {
    unsigned int manifest_version;
    unsigned int signer_key_id;
    unsigned int sig_alg;
    unsigned int flags;
    unsigned int sig_len;
    unsigned char digest[32];
    unsigned char signature[QOS_MAX_SIGNATURE_BYTES];
} kernel_manifest_t;

// Framework mode:
// 0 = warn only, continue boot on failure.
// 1 = halt boot on failure.
static const int g_kernel_verify_enforce = 0;

// Replace digest + signer metadata during provisioning.
static const kernel_manifest_t g_kernel_manifest = {
    KERNEL_MANIFEST_VERSION,
    KERNEL_MANIFEST_SIGNER_KEY_ID,
    KERNEL_MANIFEST_SIG_ALG,
    KERNEL_MANIFEST_FLAGS,
    KERNEL_MANIFEST_SIG_LEN,
    KERNEL_MANIFEST_DIGEST_INIT,
    KERNEL_MANIFEST_SIGNATURE_INIT
};

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

static int verify_manifest_policy(void){
    const trust_key_t* key = trust_find_key(g_kernel_manifest.signer_key_id);
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
    if ((key->sig_alg_mask & (1u << g_kernel_manifest.sig_alg)) == 0u){
        uart_puts("Kernel verify: signer key disallows signature algorithm.\n");
        return -1;
    }
    if (g_kernel_manifest.sig_alg != QOS_SIG_ALG_DIGEST_ONLY){
        uart_puts("Kernel verify: unsupported signature algorithm.\n");
        return -1;
    }
    if ((g_kernel_manifest.flags & QOS_PROG_FLAG_SHA256) == 0u){
        uart_puts("Kernel verify: SHA-256 flag missing.\n");
        return -1;
    }
    if (g_kernel_manifest.sig_len != 0u){
        uart_puts("Kernel verify: digest-only expects sig_len=0.\n");
        return -1;
    }
    return 0;
}

int kernel_verify_self(void){
    if (verify_manifest_policy() != 0){
        return -1;
    }

    unsigned char digest[32];
    unsigned int image_len = (unsigned int)(__kernel_rodata_end - __kernel_text_start);
    sha256_digest(__kernel_text_start, image_len, digest);

    if (digest_is_all_zero(g_kernel_manifest.digest)){
        uart_puts("Kernel verify: manifest digest not provisioned.\n");
        uart_puts("Kernel verify: measured digest=");
        uart_put_digest(digest);
        return 1;
    }

    if (!digest_equal(digest, g_kernel_manifest.digest)){
        uart_puts("Kernel verify: FAILED (digest mismatch).\n");
        uart_puts("Kernel verify: measured=");
        uart_put_digest(digest);
        uart_puts("Kernel verify: expected=");
        uart_put_digest(g_kernel_manifest.digest);
        return -1;
    }

    uart_puts("Kernel verify: OK.\n");
    return 0;
}

int kernel_verify_enforce(void){
    return g_kernel_verify_enforce;
}
