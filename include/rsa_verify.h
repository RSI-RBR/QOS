#ifndef RSA_VERIFY_H
#define RSA_VERIFY_H

#define RSA_VERIFY_MAX_MOD_BYTES 512u
#define RSA_VERIFY_MAX_EXP_BYTES 8u

int rsa_verify_x509_signature(const unsigned char* modulus,
                              unsigned int modulus_len,
                              const unsigned char* exponent,
                              unsigned int exponent_len,
                              unsigned short sig_alg,
                              const unsigned char* tbs,
                              unsigned int tbs_len,
                              const unsigned char* signature,
                              unsigned int signature_len);

#endif
