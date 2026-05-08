#include "tls_key_schedule.h"
#include "crypto.h"
#include "sha256.h"
#include "tls_record.h"

static unsigned int cstr_len_max(const char* s, unsigned int max_len){
    if (!s){
        return 0;
    }
    unsigned int n = 0;
    while (n < max_len && s[n]){
        n++;
    }
    return n;
}

int tls13_hkdf_expand_label_sha256(const unsigned char secret[TLS13_HASH_SHA256_BYTES],
                                   const char* label,
                                   const unsigned char* context, unsigned int context_len,
                                   unsigned char* out, unsigned int out_len){
    if (!secret || !label || !out || out_len == 0u){
        return -1;
    }
    if (context_len > 255u){
        return -1;
    }
    unsigned int label_len = cstr_len_max(label, 249u);
    if (label_len == 0u || label_len > 249u){
        return -1;
    }

    unsigned char info[2 + 1 + 6 + 249 + 1 + 255];
    unsigned int off = 0;
    const char prefix[] = "tls13 ";
    const unsigned int full_label_len = 6u + label_len;

    info[off++] = (unsigned char)((out_len >> 8) & 0xFFu);
    info[off++] = (unsigned char)(out_len & 0xFFu);
    info[off++] = (unsigned char)full_label_len;
    for (unsigned int i = 0; i < 6u; i++){
        info[off++] = (unsigned char)prefix[i];
    }
    for (unsigned int i = 0; i < label_len; i++){
        info[off++] = (unsigned char)label[i];
    }
    info[off++] = (unsigned char)context_len;
    for (unsigned int i = 0; i < context_len; i++){
        info[off++] = context ? context[i] : 0;
    }

    int rc = crypto_hkdf_sha256_expand(secret, info, off, out, out_len);
    crypto_memzero(info, sizeof(info));
    return rc;
}

int tls13_derive_handshake_secrets_sha256_kex(const unsigned char* kex_shared_secret,
                                              unsigned int kex_shared_secret_len,
                                              const unsigned char transcript_hash[TLS13_HASH_SHA256_BYTES],
                                              tls13_hs_secrets_t* out){
    if (!kex_shared_secret || kex_shared_secret_len == 0u || !transcript_hash || !out){
        return -1;
    }

    unsigned char zero[TLS13_HASH_SHA256_BYTES];
    unsigned char empty_hash[TLS13_HASH_SHA256_BYTES];
    unsigned char early[TLS13_HASH_SHA256_BYTES];
    unsigned char derived[TLS13_HASH_SHA256_BYTES];
    unsigned char hs_secret[TLS13_HASH_SHA256_BYTES];

    for (unsigned int i = 0; i < sizeof(zero); i++){
        zero[i] = 0;
    }
    sha256_digest(0, 0u, empty_hash);

    // TLS 1.3 key schedule (no PSK path):
    // early = Extract(0, 0)
    // derived = Expand-Label(early, "derived", "", Hash.length)
    // hs_secret = Extract(derived, ecdhe)
    // c/s hs traffic = Expand-Label(hs_secret, "c/s hs traffic", transcript_hash, Hash.length)
    // key/iv = Expand-Label(traffic_secret, "key"/"iv", "", len)
    crypto_hkdf_sha256_extract(zero, sizeof(zero), zero, sizeof(zero), early);
    if (tls13_hkdf_expand_label_sha256(early, "derived", empty_hash, sizeof(empty_hash), derived, sizeof(derived)) != 0){
        crypto_memzero(zero, sizeof(zero));
        crypto_memzero(empty_hash, sizeof(empty_hash));
        crypto_memzero(early, sizeof(early));
        crypto_memzero(derived, sizeof(derived));
        return -1;
    }
    crypto_hkdf_sha256_extract(derived, sizeof(derived),
                               kex_shared_secret, kex_shared_secret_len, hs_secret);

    if (tls13_hkdf_expand_label_sha256(hs_secret, "c hs traffic",
                                       transcript_hash, TLS13_HASH_SHA256_BYTES,
                                       out->client_hs_traffic_secret, TLS13_HASH_SHA256_BYTES) != 0){
        goto fail;
    }
    if (tls13_hkdf_expand_label_sha256(hs_secret, "s hs traffic",
                                       transcript_hash, TLS13_HASH_SHA256_BYTES,
                                       out->server_hs_traffic_secret, TLS13_HASH_SHA256_BYTES) != 0){
        goto fail;
    }
    if (tls13_hkdf_expand_label_sha256(out->client_hs_traffic_secret, "key", 0, 0,
                                       out->client_key, TLS13_KEY_BYTES) != 0){
        goto fail;
    }
    if (tls13_hkdf_expand_label_sha256(out->server_hs_traffic_secret, "key", 0, 0,
                                       out->server_key, TLS13_KEY_BYTES) != 0){
        goto fail;
    }
    if (tls13_hkdf_expand_label_sha256(out->client_hs_traffic_secret, "iv", 0, 0,
                                       out->client_iv, TLS13_IV_BYTES) != 0){
        goto fail;
    }
    if (tls13_hkdf_expand_label_sha256(out->server_hs_traffic_secret, "iv", 0, 0,
                                       out->server_iv, TLS13_IV_BYTES) != 0){
        goto fail;
    }

    crypto_memzero(zero, sizeof(zero));
    crypto_memzero(empty_hash, sizeof(empty_hash));
    crypto_memzero(early, sizeof(early));
    crypto_memzero(derived, sizeof(derived));
    crypto_memzero(hs_secret, sizeof(hs_secret));
    return 0;

fail:
    crypto_memzero(zero, sizeof(zero));
    crypto_memzero(empty_hash, sizeof(empty_hash));
    crypto_memzero(early, sizeof(early));
    crypto_memzero(derived, sizeof(derived));
    crypto_memzero(hs_secret, sizeof(hs_secret));
    crypto_memzero(out, sizeof(*out));
    return -1;
}

int tls13_derive_handshake_secrets_sha256(const unsigned char ecdhe_shared_secret[32],
                                          const unsigned char transcript_hash[TLS13_HASH_SHA256_BYTES],
                                          tls13_hs_secrets_t* out){
    return tls13_derive_handshake_secrets_sha256_kex(ecdhe_shared_secret, 32u, transcript_hash, out);
}

int tls13_key_schedule_self_test(void){
    unsigned char ecdhe[32];
    unsigned char transcript[TLS13_HASH_SHA256_BYTES];
    tls13_hs_secrets_t hs;
    tls13_record_ctx_t tx;
    tls13_record_ctx_t rx;
    unsigned char rec[128];
    unsigned int rec_len = 0;
    unsigned char plain[64];
    unsigned int plain_len = 0;
    unsigned char inner_type = 0;
    static const unsigned char msg[] = "hs-crypto-smoke";

    for (unsigned int i = 0; i < 32u; i++){
        ecdhe[i] = (unsigned char)(0xA0u + i);
    }
    sha256_digest((const unsigned char*)"transcript-seed", 15u, transcript);

    if (tls13_derive_handshake_secrets_sha256(ecdhe, transcript, &hs) != 0){
        return -1;
    }

    if (tls13_record_init(&tx, hs.client_key, TLS13_KEY_BYTES, hs.client_iv) != 0){
        return -1;
    }
    if (tls13_record_init(&rx, hs.client_key, TLS13_KEY_BYTES, hs.client_iv) != 0){
        return -1;
    }

    if (tls13_record_encrypt(&tx, 22u, msg, (unsigned int)(sizeof(msg) - 1u),
                             rec, sizeof(rec), &rec_len) != 0){
        return -1;
    }
    if (tls13_record_decrypt(&rx, rec, rec_len, plain, sizeof(plain), &plain_len, &inner_type) != 0){
        return -1;
    }
    if (inner_type != 22u){
        return -1;
    }
    if (plain_len != (unsigned int)(sizeof(msg) - 1u)){
        return -1;
    }
    if (!crypto_consttime_equal(plain, msg, plain_len)){
        return -1;
    }
    return 0;
}
