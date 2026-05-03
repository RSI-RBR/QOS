#include "kernel_verify.h"
#include "program.h"
#include "sha256.h"
#include "trust.h"
#include "uart.h"
#include "kernel_manifest_autogen.h"
#include "fat32.h"

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

// Replace digest + signer metadata during provisioning.
static const kernel_manifest_t g_kernel_manifest
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
#define KERNEL_IMAGE_MAX_SIZE (4u * 1024u * 1024u)
static unsigned char g_kernel_file_buf[KERNEL_IMAGE_MAX_SIZE];

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
    uart_puts("Kernel verify: start\n");
    if (verify_manifest_policy() != 0){
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

    if (digest_is_all_zero(g_kernel_manifest.file_digest)){
        uart_puts("Kernel file verify: manifest file digest not provisioned.\n");
        uart_puts("Kernel file verify: measured=");
        uart_put_digest(digest);
        return 1;
    }

    if (!digest_equal(digest, g_kernel_manifest.file_digest)){
        uart_puts("Kernel file verify: FAILED (digest mismatch).\n");
        uart_puts("Kernel file verify: measured=");
        uart_put_digest(digest);
        uart_puts("Kernel file verify: expected=");
        uart_put_digest(g_kernel_manifest.file_digest);
        return -1;
    }

    uart_puts("Kernel file verify: measured=");
    uart_put_digest(digest);
    uart_puts("Kernel file verify: OK.\n");
    return 0;
}
