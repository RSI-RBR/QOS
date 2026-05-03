#ifndef AES_GCM_H
#define AES_GCM_H

#define AES_GCM_BLOCK_BYTES 16u
#define AES_GCM_TAG_MAX_BYTES 16u

typedef struct {
    unsigned int rk[60];
    unsigned int nr;
    unsigned char h[AES_GCM_BLOCK_BYTES];
} aes_gcm_key_t;

int aes_gcm_key_init(aes_gcm_key_t* key,
                     const unsigned char* raw_key,
                     unsigned int raw_key_len);

int aes_gcm_encrypt(const aes_gcm_key_t* key,
                    const unsigned char* iv, unsigned int iv_len,
                    const unsigned char* aad, unsigned int aad_len,
                    const unsigned char* pt, unsigned int pt_len,
                    unsigned char* ct,
                    unsigned char* tag, unsigned int tag_len);

int aes_gcm_decrypt(const aes_gcm_key_t* key,
                    const unsigned char* iv, unsigned int iv_len,
                    const unsigned char* aad, unsigned int aad_len,
                    const unsigned char* ct, unsigned int ct_len,
                    unsigned char* pt,
                    const unsigned char* tag, unsigned int tag_len);

int aes_gcm_self_test(void);

#endif
