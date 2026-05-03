#ifndef TLS_KEY_SCHEDULE_H
#define TLS_KEY_SCHEDULE_H

#define TLS13_HASH_SHA256_BYTES 32u
#define TLS13_KEY_BYTES 16u
#define TLS13_IV_BYTES 12u

typedef struct {
    unsigned char client_hs_traffic_secret[TLS13_HASH_SHA256_BYTES];
    unsigned char server_hs_traffic_secret[TLS13_HASH_SHA256_BYTES];
    unsigned char client_key[TLS13_KEY_BYTES];
    unsigned char server_key[TLS13_KEY_BYTES];
    unsigned char client_iv[TLS13_IV_BYTES];
    unsigned char server_iv[TLS13_IV_BYTES];
} tls13_hs_secrets_t;

int tls13_hkdf_expand_label_sha256(const unsigned char secret[TLS13_HASH_SHA256_BYTES],
                                   const char* label,
                                   const unsigned char* context, unsigned int context_len,
                                   unsigned char* out, unsigned int out_len);

int tls13_derive_handshake_secrets_sha256(const unsigned char ecdhe_shared_secret[32],
                                          const unsigned char transcript_hash[TLS13_HASH_SHA256_BYTES],
                                          tls13_hs_secrets_t* out);

int tls13_key_schedule_self_test(void);

#endif
