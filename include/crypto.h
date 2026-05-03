#ifndef CRYPTO_H
#define CRYPTO_H

#define CRYPTO_KEY_BYTES 32u
#define CRYPTO_NONCE_BYTES 12u

#define CRYPTO_SCOPE_DISK   0x00000001u
#define CRYPTO_SCOPE_NET    0x00000002u
#define CRYPTO_SCOPE_APP    0x00000004u
#define CRYPTO_SCOPE_SYSTEM 0x00000008u

#define CRYPTO_KEY_ID_SYSTEM_EPHEMERAL 0xFFFF0001u

void crypto_init(void);
void crypto_add_entropy(const void* data, unsigned int len);
int crypto_random_bytes(unsigned char* out, unsigned int len);

void crypto_hmac_sha256(const unsigned char* key, unsigned int key_len,
                        const unsigned char* data, unsigned int data_len,
                        unsigned char out[32]);

void crypto_hkdf_sha256_extract(const unsigned char* salt, unsigned int salt_len,
                                const unsigned char* ikm, unsigned int ikm_len,
                                unsigned char prk_out[32]);

int crypto_hkdf_sha256_expand(const unsigned char prk[32],
                              const unsigned char* info, unsigned int info_len,
                              unsigned char* out, unsigned int out_len);

int crypto_import_key(unsigned int key_id, unsigned int scopes,
                      const unsigned char* key, unsigned int key_len);
int crypto_remove_key(unsigned int key_id);
int crypto_has_key(unsigned int key_id);

int crypto_derive_key(unsigned int key_id, unsigned int scope,
                      const char* label,
                      const void* context, unsigned int context_len,
                      unsigned char* out, unsigned int out_len);

int crypto_next_nonce(unsigned int key_id, unsigned int scope,
                      unsigned char out_nonce[CRYPTO_NONCE_BYTES]);

int crypto_derive_disk_key(unsigned int key_id,
                           unsigned int disk_id,
                           unsigned long long block_index,
                           unsigned char out_key[CRYPTO_KEY_BYTES]);

int crypto_derive_net_key(unsigned int key_id,
                          unsigned int local_ip_be,
                          unsigned int remote_ip_be,
                          unsigned short local_port_be,
                          unsigned short remote_port_be,
                          unsigned char out_key[CRYPTO_KEY_BYTES]);

void crypto_memzero(void* p, unsigned int len);
int crypto_consttime_equal(const unsigned char* a, const unsigned char* b, unsigned int len);

#endif
