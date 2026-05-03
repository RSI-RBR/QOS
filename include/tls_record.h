#ifndef TLS_RECORD_H
#define TLS_RECORD_H

#include "aes_gcm.h"

#define TLS13_AEAD_KEY_BYTES 16u
#define TLS13_AEAD_IV_BYTES 12u
#define TLS13_TAG_BYTES 16u
#define TLS13_RECORD_MAX_PLAINTEXT 16384u
#define TLS13_RECORD_MAX_CIPHERTEXT (TLS13_RECORD_MAX_PLAINTEXT + 1u + TLS13_TAG_BYTES)
#define TLS13_RECORD_HEADER_BYTES 5u

typedef struct {
    aes_gcm_key_t aead_key;
    unsigned char static_iv[TLS13_AEAD_IV_BYTES];
    unsigned long long seq;
    int ready;
} tls13_record_ctx_t;

int tls13_record_init(tls13_record_ctx_t* ctx,
                      const unsigned char* key, unsigned int key_len,
                      const unsigned char iv[TLS13_AEAD_IV_BYTES]);

void tls13_record_reset_seq(tls13_record_ctx_t* ctx, unsigned long long seq);

int tls13_record_encrypt(tls13_record_ctx_t* ctx,
                         unsigned char inner_content_type,
                         const unsigned char* plaintext, unsigned int plaintext_len,
                         unsigned char* out_record, unsigned int out_cap, unsigned int* out_len);

int tls13_record_decrypt(tls13_record_ctx_t* ctx,
                         const unsigned char* record, unsigned int record_len,
                         unsigned char* out_plaintext, unsigned int out_cap, unsigned int* out_len,
                         unsigned char* out_inner_content_type);

int tls13_record_self_test(void);

#endif
