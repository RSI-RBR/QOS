#ifndef SHA256_H
#define SHA256_H

typedef struct {
    unsigned int h[8];
    unsigned long long bitlen;
    unsigned int datalen;
    unsigned char data[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const unsigned char* data, unsigned int len);
void sha256_final(sha256_ctx_t* ctx, unsigned char out[32]);

void sha256_digest(const unsigned char* data, unsigned int len, unsigned char out[32]);
void sha256_digest_concat2(const unsigned char* a, unsigned int a_len,
                           const unsigned char* b, unsigned int b_len,
                           unsigned char out[32]);

#endif
