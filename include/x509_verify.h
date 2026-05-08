#ifndef X509_VERIFY_H
#define X509_VERIFY_H

#include "rsa_verify.h"

#define X509_VERIFY_KEY_NONE 0u
#define X509_VERIFY_KEY_RSA 1u

typedef struct {
    unsigned int chain_certs;
    unsigned int anchor_count;
    unsigned short leaf_cert_sig_alg;
    unsigned int leaf_key_alg;
    unsigned char leaf_rsa_n[RSA_VERIFY_MAX_MOD_BYTES];
    unsigned int leaf_rsa_n_len;
    unsigned char leaf_rsa_e[RSA_VERIFY_MAX_EXP_BYTES];
    unsigned int leaf_rsa_e_len;
    int hostname_ok;
    int chain_anchor_ok;
} x509_verify_result_t;

void x509_verify_reset_cache(void);

// cert_body points to TLS 1.3 Certificate handshake body:
//   opaque certificate_request_context<0..2^8-1>;
//   CertificateEntry certificate_list<0..2^24-1>;
int x509_verify_tls13_certificate(const char* host,
                                  const unsigned char* cert_body,
                                  unsigned int cert_body_len,
                                  unsigned short server_cert_verify_alg,
                                  x509_verify_result_t* out_result);

#endif
