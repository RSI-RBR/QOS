#ifndef SHA256_H
#define SHA256_H

void sha256_digest(const unsigned char* data, unsigned int len, unsigned char out[32]);
void sha256_digest_concat2(const unsigned char* a, unsigned int a_len,
                           const unsigned char* b, unsigned int b_len,
                           unsigned char out[32]);

#endif
