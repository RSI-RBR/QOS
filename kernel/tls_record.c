#include "tls_record.h"
#include "crypto.h"

#define TLS13_OUTER_CONTENT_TYPE_APPLICATION_DATA 23u
#define TLS_LEGACY_RECORD_VERSION_MAJOR 0x03u
#define TLS_LEGACY_RECORD_VERSION_MINOR 0x03u

static void store_be16(unsigned char out[2], unsigned int v){
    out[0] = (unsigned char)((v >> 8) & 0xFFu);
    out[1] = (unsigned char)(v & 0xFFu);
}

static unsigned int load_be16(const unsigned char in[2]){
    return ((unsigned int)in[0] << 8) | (unsigned int)in[1];
}

static void seq_to_nonce(const tls13_record_ctx_t* ctx, unsigned char nonce[12]){
    for (unsigned int i = 0; i < 12u; i++){
        nonce[i] = ctx->static_iv[i];
    }
    // TLS 1.3 nonce: static_iv XOR (0...0 || seq_num_be64)
    for (unsigned int i = 0; i < 8u; i++){
        unsigned int shift = (unsigned int)(56u - (8u * i));
        unsigned char seq_b = (unsigned char)((ctx->seq >> shift) & 0xFFu);
        nonce[4u + i] ^= seq_b;
    }
}

int tls13_record_init(tls13_record_ctx_t* ctx,
                      const unsigned char* key, unsigned int key_len,
                      const unsigned char iv[TLS13_AEAD_IV_BYTES]){
    if (!ctx || !key || !iv){
        return -1;
    }
    if (key_len != TLS13_AEAD_KEY_BYTES && key_len != 32u){
        return -1;
    }
    if (aes_gcm_key_init(&ctx->aead_key, key, key_len) != 0){
        return -1;
    }
    for (unsigned int i = 0; i < TLS13_AEAD_IV_BYTES; i++){
        ctx->static_iv[i] = iv[i];
    }
    ctx->seq = 0;
    ctx->ready = 1;
    return 0;
}

void tls13_record_reset_seq(tls13_record_ctx_t* ctx, unsigned long long seq){
    if (!ctx){
        return;
    }
    ctx->seq = seq;
}

int tls13_record_encrypt(tls13_record_ctx_t* ctx,
                         unsigned char inner_content_type,
                         const unsigned char* plaintext, unsigned int plaintext_len,
                         unsigned char* out_record, unsigned int out_cap, unsigned int* out_len){
    if (!ctx || !ctx->ready || !out_record || !out_len){
        return -1;
    }
    if (plaintext_len > 0u && !plaintext){
        return -1;
    }
    if (plaintext_len > TLS13_RECORD_MAX_PLAINTEXT){
        return -1;
    }

    unsigned int inner_len = plaintext_len + 1u;
    unsigned int ct_len = inner_len;
    unsigned int wire_len = TLS13_RECORD_HEADER_BYTES + ct_len + TLS13_TAG_BYTES;
    if (wire_len > out_cap){
        return -1;
    }

    unsigned char aad[TLS13_RECORD_HEADER_BYTES];
    unsigned char nonce[12];
    unsigned char inner[TLS13_RECORD_MAX_PLAINTEXT + 1u];
    unsigned char tag[TLS13_TAG_BYTES];

    aad[0] = TLS13_OUTER_CONTENT_TYPE_APPLICATION_DATA;
    aad[1] = TLS_LEGACY_RECORD_VERSION_MAJOR;
    aad[2] = TLS_LEGACY_RECORD_VERSION_MINOR;
    store_be16(&aad[3], ct_len + TLS13_TAG_BYTES);

    for (unsigned int i = 0; i < plaintext_len; i++){
        inner[i] = plaintext[i];
    }
    inner[plaintext_len] = inner_content_type;

    seq_to_nonce(ctx, nonce);

    out_record[0] = aad[0];
    out_record[1] = aad[1];
    out_record[2] = aad[2];
    out_record[3] = aad[3];
    out_record[4] = aad[4];

    if (aes_gcm_encrypt(&ctx->aead_key,
                        nonce, sizeof(nonce),
                        aad, sizeof(aad),
                        inner, inner_len,
                        &out_record[TLS13_RECORD_HEADER_BYTES],
                        tag, sizeof(tag)) != 0){
        crypto_memzero(nonce, sizeof(nonce));
        crypto_memzero(inner, sizeof(inner));
        crypto_memzero(tag, sizeof(tag));
        return -1;
    }
    for (unsigned int i = 0; i < TLS13_TAG_BYTES; i++){
        out_record[TLS13_RECORD_HEADER_BYTES + ct_len + i] = tag[i];
    }

    ctx->seq++;
    *out_len = wire_len;
    crypto_memzero(nonce, sizeof(nonce));
    crypto_memzero(inner, sizeof(inner));
    crypto_memzero(tag, sizeof(tag));
    return 0;
}

int tls13_record_decrypt(tls13_record_ctx_t* ctx,
                         const unsigned char* record, unsigned int record_len,
                         unsigned char* out_plaintext, unsigned int out_cap, unsigned int* out_len,
                         unsigned char* out_inner_content_type){
    if (!ctx || !ctx->ready || !record || !out_plaintext || !out_len || !out_inner_content_type){
        return -1;
    }
    if (record_len < (TLS13_RECORD_HEADER_BYTES + TLS13_TAG_BYTES + 1u)){
        return -1;
    }
    if (record[0] != TLS13_OUTER_CONTENT_TYPE_APPLICATION_DATA){
        return -1;
    }
    if (record[1] != TLS_LEGACY_RECORD_VERSION_MAJOR ||
        record[2] != TLS_LEGACY_RECORD_VERSION_MINOR){
        return -1;
    }

    unsigned int body_len = load_be16(&record[3]);
    if (body_len + TLS13_RECORD_HEADER_BYTES != record_len){
        return -1;
    }
    if (body_len < (TLS13_TAG_BYTES + 1u) || body_len > TLS13_RECORD_MAX_CIPHERTEXT){
        return -1;
    }

    unsigned int ct_len = body_len - TLS13_TAG_BYTES;
    if (ct_len == 0u){
        return -1;
    }

    unsigned char aad[TLS13_RECORD_HEADER_BYTES];
    unsigned char nonce[12];
    unsigned char inner[TLS13_RECORD_MAX_PLAINTEXT + 1u];

    for (unsigned int i = 0; i < TLS13_RECORD_HEADER_BYTES; i++){
        aad[i] = record[i];
    }
    seq_to_nonce(ctx, nonce);

    const unsigned char* ct = &record[TLS13_RECORD_HEADER_BYTES];
    const unsigned char* tag = &record[TLS13_RECORD_HEADER_BYTES + ct_len];

    if (aes_gcm_decrypt(&ctx->aead_key,
                        nonce, sizeof(nonce),
                        aad, sizeof(aad),
                        ct, ct_len,
                        inner,
                        tag, TLS13_TAG_BYTES) != 0){
        crypto_memzero(nonce, sizeof(nonce));
        crypto_memzero(inner, sizeof(inner));
        return -1;
    }

    unsigned int inner_len = ct_len;
    if (inner_len < 1u){
        crypto_memzero(nonce, sizeof(nonce));
        crypto_memzero(inner, sizeof(inner));
        return -1;
    }
    unsigned char inner_type = inner[inner_len - 1u];
    unsigned int plain_len = inner_len - 1u;
    if (plain_len > out_cap){
        crypto_memzero(nonce, sizeof(nonce));
        crypto_memzero(inner, sizeof(inner));
        return -1;
    }
    for (unsigned int i = 0; i < plain_len; i++){
        out_plaintext[i] = inner[i];
    }

    *out_inner_content_type = inner_type;
    *out_len = plain_len;
    ctx->seq++;
    crypto_memzero(nonce, sizeof(nonce));
    crypto_memzero(inner, sizeof(inner));
    return 0;
}

int tls13_record_self_test(void){
    static const unsigned char key[16] = {
        0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
        0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F
    };
    static const unsigned char iv[12] = {
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B
    };
    static const unsigned char msg[] = "hello tls record";

    tls13_record_ctx_t tx;
    tls13_record_ctx_t rx;
    unsigned char rec[256];
    unsigned int rec_len = 0;
    unsigned char out[256];
    unsigned int out_len = 0;
    unsigned char inner_type = 0;

    if (tls13_record_init(&tx, key, sizeof(key), iv) != 0){
        return -1;
    }
    if (tls13_record_init(&rx, key, sizeof(key), iv) != 0){
        return -1;
    }

    if (tls13_record_encrypt(&tx, 23u, msg, (unsigned int)(sizeof(msg) - 1u),
                             rec, sizeof(rec), &rec_len) != 0){
        return -1;
    }
    if (tls13_record_decrypt(&rx, rec, rec_len, out, sizeof(out), &out_len, &inner_type) != 0){
        return -1;
    }
    if (inner_type != 23u){
        return -1;
    }
    if (out_len != (unsigned int)(sizeof(msg) - 1u)){
        return -1;
    }
    if (!crypto_consttime_equal(out, msg, out_len)){
        return -1;
    }
    return 0;
}
