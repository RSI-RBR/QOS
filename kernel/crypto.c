#include "crypto.h"
#include "sha256.h"
#include "spinlock.h"
#include "timer.h"

#define CRYPTO_MAX_KEY_SLOTS 16u
#define CRYPTO_ALLOWED_SCOPES (CRYPTO_SCOPE_DISK | CRYPTO_SCOPE_NET | CRYPTO_SCOPE_APP | CRYPTO_SCOPE_SYSTEM)
#define CRYPTO_MAX_LABEL_LEN 48u
#define CRYPTO_MAX_CONTEXT_LEN 128u

typedef struct {
    unsigned char k[32];
    unsigned char v[32];
    unsigned long reseed_count;
    int seeded;
} hmac_drbg_t;

typedef struct {
    unsigned int valid;
    unsigned int key_id;
    unsigned int scopes;
    unsigned long long nonce_counter;
    unsigned char key[CRYPTO_KEY_BYTES];
} crypto_key_slot_t;

static spinlock_t g_crypto_lock;
static hmac_drbg_t g_drbg;
static crypto_key_slot_t g_key_slots[CRYPTO_MAX_KEY_SLOTS];
static int g_crypto_init_done = 0;

static void store_u32_le(unsigned char* out, unsigned int v){
    out[0] = (unsigned char)(v & 0xFFu);
    out[1] = (unsigned char)((v >> 8) & 0xFFu);
    out[2] = (unsigned char)((v >> 16) & 0xFFu);
    out[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void store_u64_le(unsigned char* out, unsigned long long v){
    for (unsigned int i = 0; i < 8u; i++){
        out[i] = (unsigned char)((v >> (8u * i)) & 0xFFu);
    }
}

static unsigned int cstrnlen_local(const char* s, unsigned int max_len){
    if (!s){
        return 0;
    }
    unsigned int n = 0;
    while (n < max_len && s[n]){
        n++;
    }
    return n;
}

static void key_slots_zero_all_locked(void){
    for (unsigned int i = 0; i < CRYPTO_MAX_KEY_SLOTS; i++){
        g_key_slots[i].valid = 0;
        g_key_slots[i].key_id = 0;
        g_key_slots[i].scopes = 0;
        g_key_slots[i].nonce_counter = 0;
        crypto_memzero(g_key_slots[i].key, sizeof(g_key_slots[i].key));
    }
}

static int key_slot_find_index_locked(unsigned int key_id){
    for (unsigned int i = 0; i < CRYPTO_MAX_KEY_SLOTS; i++){
        if (g_key_slots[i].valid && g_key_slots[i].key_id == key_id){
            return (int)i;
        }
    }
    return -1;
}

static int key_slot_alloc_index_locked(void){
    for (unsigned int i = 0; i < CRYPTO_MAX_KEY_SLOTS; i++){
        if (!g_key_slots[i].valid){
            return (int)i;
        }
    }
    return -1;
}

static int key_slot_import_locked(unsigned int key_id, unsigned int scopes,
                                  const unsigned char normalized_key[CRYPTO_KEY_BYTES]){
    if (!normalized_key){
        return -1;
    }
    scopes &= CRYPTO_ALLOWED_SCOPES;
    if (scopes == 0u){
        return -1;
    }

    int idx = key_slot_find_index_locked(key_id);
    if (idx < 0){
        idx = key_slot_alloc_index_locked();
    }
    if (idx < 0){
        return -1;
    }

    g_key_slots[idx].valid = 1;
    g_key_slots[idx].key_id = key_id;
    g_key_slots[idx].scopes = scopes;
    g_key_slots[idx].nonce_counter = 0;
    for (unsigned int i = 0; i < CRYPTO_KEY_BYTES; i++){
        g_key_slots[idx].key[i] = normalized_key[i];
    }
    return 0;
}

static int key_slot_remove_locked(unsigned int key_id){
    int idx = key_slot_find_index_locked(key_id);
    if (idx < 0){
        return -1;
    }
    crypto_memzero(g_key_slots[idx].key, sizeof(g_key_slots[idx].key));
    g_key_slots[idx].valid = 0;
    g_key_slots[idx].key_id = 0;
    g_key_slots[idx].scopes = 0;
    g_key_slots[idx].nonce_counter = 0;
    return 0;
}

void crypto_memzero(void* p, unsigned int len){
    volatile unsigned char* v = (volatile unsigned char*)p;
    for (unsigned int i = 0; i < len; i++){
        v[i] = 0;
    }
}

int crypto_consttime_equal(const unsigned char* a, const unsigned char* b, unsigned int len){
    if (!a || !b){
        return 0;
    }
    unsigned char diff = 0;
    for (unsigned int i = 0; i < len; i++){
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

void crypto_hmac_sha256(const unsigned char* key, unsigned int key_len,
                        const unsigned char* data, unsigned int data_len,
                        unsigned char out[32]){
    unsigned char kh[32];
    unsigned char kipad[64];
    unsigned char kopad[64];
    unsigned char keyblk[64];
    sha256_ctx_t ctx;

    for (unsigned int i = 0; i < 64; i++){
        keyblk[i] = 0;
    }

    if (key && key_len > 64u){
        sha256_digest(key, key_len, kh);
        for (unsigned int i = 0; i < 32u; i++){
            keyblk[i] = kh[i];
        }
        crypto_memzero(kh, sizeof(kh));
    } else if (key && key_len){
        for (unsigned int i = 0; i < key_len; i++){
            keyblk[i] = key[i];
        }
    }

    for (unsigned int i = 0; i < 64u; i++){
        kipad[i] = (unsigned char)(keyblk[i] ^ 0x36u);
        kopad[i] = (unsigned char)(keyblk[i] ^ 0x5Cu);
    }

    sha256_init(&ctx);
    sha256_update(&ctx, kipad, sizeof(kipad));
    if (data && data_len){
        sha256_update(&ctx, data, data_len);
    }
    sha256_final(&ctx, kh);

    sha256_init(&ctx);
    sha256_update(&ctx, kopad, sizeof(kopad));
    sha256_update(&ctx, kh, sizeof(kh));
    sha256_final(&ctx, out);

    crypto_memzero(kh, sizeof(kh));
    crypto_memzero(keyblk, sizeof(keyblk));
    crypto_memzero(kipad, sizeof(kipad));
    crypto_memzero(kopad, sizeof(kopad));
}

void crypto_hkdf_sha256_extract(const unsigned char* salt, unsigned int salt_len,
                                const unsigned char* ikm, unsigned int ikm_len,
                                unsigned char prk_out[32]){
    unsigned char zero_salt[32];
    if (!prk_out){
        return;
    }
    if (!salt || salt_len == 0u){
        for (unsigned int i = 0; i < sizeof(zero_salt); i++){
            zero_salt[i] = 0;
        }
        crypto_hmac_sha256(zero_salt, sizeof(zero_salt), ikm, ikm_len, prk_out);
        crypto_memzero(zero_salt, sizeof(zero_salt));
        return;
    }
    crypto_hmac_sha256(salt, salt_len, ikm, ikm_len, prk_out);
}

int crypto_hkdf_sha256_expand(const unsigned char prk[32],
                              const unsigned char* info, unsigned int info_len,
                              unsigned char* out, unsigned int out_len){
    if (!prk || !out){
        return -1;
    }
    if (out_len == 0u){
        return 0;
    }
    if (out_len > (255u * 32u)){
        return -1;
    }

    unsigned char t[32];
    unsigned int t_len = 0;
    unsigned int produced = 0;
    unsigned char counter = 1;

    while (produced < out_len){
        unsigned char block[32 + 255 + 1];
        unsigned int blen = 0;
        for (unsigned int i = 0; i < t_len; i++){
            block[blen++] = t[i];
        }
        if (info && info_len){
            for (unsigned int i = 0; i < info_len; i++){
                block[blen++] = info[i];
            }
        }
        block[blen++] = counter;

        crypto_hmac_sha256(prk, 32u, block, blen, t);
        t_len = 32u;

        unsigned int need = out_len - produced;
        unsigned int take = (need < 32u) ? need : 32u;
        for (unsigned int i = 0; i < take; i++){
            out[produced + i] = t[i];
        }
        produced += take;
        counter++;
    }

    crypto_memzero(t, sizeof(t));
    return 0;
}

static void drbg_hmac_update(const unsigned char* provided, unsigned int provided_len){
    unsigned char temp[32 + 1 + 32];
    unsigned char provided_hash[32];
    const unsigned char* p = provided;
    unsigned int p_len = provided_len;
    unsigned int tlen = 0;

    if (provided && provided_len > 32u){
        sha256_digest(provided, provided_len, provided_hash);
        p = provided_hash;
        p_len = sizeof(provided_hash);
    }

    for (unsigned int i = 0; i < 32u; i++){
        temp[tlen++] = g_drbg.v[i];
    }
    temp[tlen++] = 0x00u;
    if (p && p_len){
        for (unsigned int i = 0; i < p_len; i++){
            temp[tlen++] = p[i];
        }
    }
    crypto_hmac_sha256(g_drbg.k, 32u, temp, tlen, g_drbg.k);
    crypto_hmac_sha256(g_drbg.k, 32u, g_drbg.v, 32u, g_drbg.v);

    if (p && p_len){
        tlen = 0;
        for (unsigned int i = 0; i < 32u; i++){
            temp[tlen++] = g_drbg.v[i];
        }
        temp[tlen++] = 0x01u;
        for (unsigned int i = 0; i < p_len; i++){
            temp[tlen++] = p[i];
        }
        crypto_hmac_sha256(g_drbg.k, 32u, temp, tlen, g_drbg.k);
        crypto_hmac_sha256(g_drbg.k, 32u, g_drbg.v, 32u, g_drbg.v);
    }

    crypto_memzero(provided_hash, sizeof(provided_hash));
    crypto_memzero(temp, sizeof(temp));
}

static void drbg_reseed(const unsigned char* seed, unsigned int seed_len){
    if (!seed || seed_len == 0u){
        return;
    }
    drbg_hmac_update(seed, seed_len);
    g_drbg.reseed_count = 1;
    g_drbg.seeded = 1;
}

void crypto_add_entropy(const void* data, unsigned int len){
    if (!data || len == 0u){
        return;
    }
    if (!g_crypto_init_done){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    drbg_reseed((const unsigned char*)data, len);
    spin_unlock_irqrestore(&g_crypto_lock, irq);
}

void crypto_init(void){
    spinlock_init(&g_crypto_lock);

    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    for (unsigned int i = 0; i < 32u; i++){
        g_drbg.k[i] = 0x00u;
        g_drbg.v[i] = 0x01u;
    }
    g_drbg.reseed_count = 0;
    g_drbg.seeded = 0;
    key_slots_zero_all_locked();
    g_crypto_init_done = 1;

    unsigned char seed[64];
    unsigned int off = 0;
    unsigned long c = 0;
    unsigned long f = 0;
    unsigned long sp = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(c));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    asm volatile("mov %0, sp" : "=r"(sp));

    unsigned long vals[6];
    vals[0] = c;
    vals[1] = f;
    vals[2] = (unsigned long)system_ticks;
    vals[3] = sp;
    vals[4] = (unsigned long)(unsigned long long)&g_drbg;
    vals[5] = (unsigned long)(unsigned long long)&crypto_init;

    for (unsigned int i = 0; i < 6u; i++){
        unsigned long v = vals[i];
        for (unsigned int b = 0; b < sizeof(unsigned long); b++){
            seed[off++] = (unsigned char)((v >> (8u * b)) & 0xFFu);
            if (off >= sizeof(seed)){
                break;
            }
        }
        if (off >= sizeof(seed)){
            break;
        }
    }

    drbg_reseed(seed, off);

    // Install a boot-ephemeral system master key for scoped derivation.
    // This enables immediate disk/net key scaffolding even before persistent
    // key provisioning is added.
    unsigned char sys_master[CRYPTO_KEY_BYTES];
    crypto_hmac_sha256(g_drbg.k, sizeof(g_drbg.k), g_drbg.v, sizeof(g_drbg.v), sys_master);
    (void)key_slot_import_locked(CRYPTO_KEY_ID_SYSTEM_EPHEMERAL,
                                 CRYPTO_SCOPE_DISK | CRYPTO_SCOPE_NET | CRYPTO_SCOPE_SYSTEM,
                                 sys_master);
    crypto_memzero(sys_master, sizeof(sys_master));

    crypto_memzero(seed, sizeof(seed));
    spin_unlock_irqrestore(&g_crypto_lock, irq);
}

int crypto_random_bytes(unsigned char* out, unsigned int len){
    if (!out){
        return -1;
    }
    if (len == 0u){
        return 0;
    }
    if (!g_crypto_init_done){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    if (!g_drbg.seeded){
        spin_unlock_irqrestore(&g_crypto_lock, irq);
        return -1;
    }

    unsigned int produced = 0;
    while (produced < len){
        crypto_hmac_sha256(g_drbg.k, 32u, g_drbg.v, 32u, g_drbg.v);
        unsigned int need = len - produced;
        unsigned int take = (need < 32u) ? need : 32u;
        for (unsigned int i = 0; i < take; i++){
            out[produced + i] = g_drbg.v[i];
        }
        produced += take;
    }

    drbg_hmac_update(0, 0);
    g_drbg.reseed_count++;
    spin_unlock_irqrestore(&g_crypto_lock, irq);
    return 0;
}

int crypto_import_key(unsigned int key_id, unsigned int scopes,
                      const unsigned char* key, unsigned int key_len){
    if (!key || key_len == 0u){
        return -1;
    }
    if (!g_crypto_init_done){
        return -1;
    }

    unsigned char norm[CRYPTO_KEY_BYTES];
    static const unsigned char import_salt[] = "QOS-KEY-IMPORT-v1";
    crypto_hkdf_sha256_extract(import_salt, (unsigned int)(sizeof(import_salt) - 1u),
                               key, key_len, norm);

    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    int rc = key_slot_import_locked(key_id, scopes, norm);
    spin_unlock_irqrestore(&g_crypto_lock, irq);

    crypto_memzero(norm, sizeof(norm));
    return rc;
}

int crypto_remove_key(unsigned int key_id){
    if (!g_crypto_init_done){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    int rc = key_slot_remove_locked(key_id);
    spin_unlock_irqrestore(&g_crypto_lock, irq);
    return rc;
}

int crypto_has_key(unsigned int key_id){
    if (!g_crypto_init_done){
        return 0;
    }
    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    int idx = key_slot_find_index_locked(key_id);
    spin_unlock_irqrestore(&g_crypto_lock, irq);
    return idx >= 0;
}

static int crypto_derive_key_locked(unsigned int key_id, unsigned int scope,
                                    const char* label,
                                    const void* context, unsigned int context_len,
                                    unsigned char* out, unsigned int out_len){
    if (!out || out_len == 0u || !label){
        return -1;
    }
    if ((scope & CRYPTO_ALLOWED_SCOPES) == 0u){
        return -1;
    }
    if (context_len > CRYPTO_MAX_CONTEXT_LEN){
        return -1;
    }

    int idx = key_slot_find_index_locked(key_id);
    if (idx < 0){
        return -1;
    }
    if ((g_key_slots[idx].scopes & scope) == 0u){
        return -1;
    }

    unsigned int label_len = cstrnlen_local(label, CRYPTO_MAX_LABEL_LEN + 1u);
    if (label_len == 0u || label_len > CRYPTO_MAX_LABEL_LEN){
        return -1;
    }

    unsigned char salt[16];
    unsigned char prk[32];
    unsigned char info[32 + CRYPTO_MAX_LABEL_LEN + CRYPTO_MAX_CONTEXT_LEN];
    unsigned int info_len = 0;

    store_u32_le(&salt[0], 0x3144464Bu); // "KFD1"
    store_u32_le(&salt[4], key_id);
    store_u32_le(&salt[8], scope);
    store_u32_le(&salt[12], out_len);
    crypto_hkdf_sha256_extract(salt, sizeof(salt),
                               g_key_slots[idx].key, CRYPTO_KEY_BYTES, prk);

    store_u32_le(&info[info_len], 0x2D534F51u); info_len += 4; // "QOS-"
    store_u32_le(&info[info_len], 0x2D46444Bu); info_len += 4; // "KDF-"
    store_u32_le(&info[info_len], 0x00003176u); info_len += 4; // "v1\0"
    store_u32_le(&info[info_len], key_id); info_len += 4;
    store_u32_le(&info[info_len], scope); info_len += 4;
    info[info_len++] = (unsigned char)label_len;
    for (unsigned int i = 0; i < label_len; i++){
        info[info_len++] = (unsigned char)label[i];
    }
    if (context && context_len){
        const unsigned char* ctx = (const unsigned char*)context;
        for (unsigned int i = 0; i < context_len; i++){
            info[info_len++] = ctx[i];
        }
    }

    int rc = crypto_hkdf_sha256_expand(prk, info, info_len, out, out_len);
    crypto_memzero(salt, sizeof(salt));
    crypto_memzero(prk, sizeof(prk));
    crypto_memzero(info, sizeof(info));
    return rc;
}

int crypto_derive_key(unsigned int key_id, unsigned int scope,
                      const char* label,
                      const void* context, unsigned int context_len,
                      unsigned char* out, unsigned int out_len){
    if (!g_crypto_init_done){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    int rc = crypto_derive_key_locked(key_id, scope, label, context, context_len, out, out_len);
    spin_unlock_irqrestore(&g_crypto_lock, irq);
    return rc;
}

int crypto_next_nonce(unsigned int key_id, unsigned int scope,
                      unsigned char out_nonce[CRYPTO_NONCE_BYTES]){
    if (!out_nonce || !g_crypto_init_done){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_crypto_lock);
    int idx = key_slot_find_index_locked(key_id);
    if (idx < 0 || (g_key_slots[idx].scopes & scope) == 0u){
        spin_unlock_irqrestore(&g_crypto_lock, irq);
        return -1;
    }

    g_key_slots[idx].nonce_counter++;
    unsigned long long ctr = g_key_slots[idx].nonce_counter;

    unsigned char nonce_ctx[16];
    unsigned char digest[32];
    store_u32_le(&nonce_ctx[0], key_id);
    store_u32_le(&nonce_ctx[4], scope);
    store_u64_le(&nonce_ctx[8], ctr);
    crypto_hmac_sha256(g_key_slots[idx].key, CRYPTO_KEY_BYTES, nonce_ctx, sizeof(nonce_ctx), digest);
    for (unsigned int i = 0; i < CRYPTO_NONCE_BYTES; i++){
        out_nonce[i] = digest[i];
    }
    crypto_memzero(nonce_ctx, sizeof(nonce_ctx));
    crypto_memzero(digest, sizeof(digest));
    spin_unlock_irqrestore(&g_crypto_lock, irq);
    return 0;
}

int crypto_derive_disk_key(unsigned int key_id,
                           unsigned int disk_id,
                           unsigned long long block_index,
                           unsigned char out_key[CRYPTO_KEY_BYTES]){
    unsigned char ctx[12];
    store_u32_le(&ctx[0], disk_id);
    store_u64_le(&ctx[4], block_index);
    return crypto_derive_key(key_id, CRYPTO_SCOPE_DISK, "disk-block-v1",
                             ctx, sizeof(ctx), out_key, CRYPTO_KEY_BYTES);
}

int crypto_derive_net_key(unsigned int key_id,
                          unsigned int local_ip_be,
                          unsigned int remote_ip_be,
                          unsigned short local_port_be,
                          unsigned short remote_port_be,
                          unsigned char out_key[CRYPTO_KEY_BYTES]){
    unsigned char ctx[12];
    store_u32_le(&ctx[0], local_ip_be);
    store_u32_le(&ctx[4], remote_ip_be);
    ctx[8] = (unsigned char)((local_port_be >> 8) & 0xFFu);
    ctx[9] = (unsigned char)(local_port_be & 0xFFu);
    ctx[10] = (unsigned char)((remote_port_be >> 8) & 0xFFu);
    ctx[11] = (unsigned char)(remote_port_be & 0xFFu);
    return crypto_derive_key(key_id, CRYPTO_SCOPE_NET, "net-flow-v1",
                             ctx, sizeof(ctx), out_key, CRYPTO_KEY_BYTES);
}
