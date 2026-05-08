#ifndef ECDSA_VERIFY_H
#define ECDSA_VERIFY_H

#define ECDSA_VERIFY_CURVE_NONE 0u
#define ECDSA_VERIFY_CURVE_P256 1u
#define ECDSA_VERIFY_CURVE_P384 2u

int ecdsa_verify_signature(unsigned int curve_id,
                           const unsigned char* pub_x,
                           const unsigned char* pub_y,
                           unsigned int pub_len,
                           unsigned short sig_alg,
                           const unsigned char* message,
                           unsigned int message_len,
                           const unsigned char* der_signature,
                           unsigned int der_signature_len);

#endif
