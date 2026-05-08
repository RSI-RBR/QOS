#ifndef PQ_KEM_H
#define PQ_KEM_H

#define QOS_KEM_MLKEM768_PK_BYTES 1184u
#define QOS_KEM_MLKEM768_SK_BYTES 2400u
#define QOS_KEM_MLKEM768_CT_BYTES 1088u
#define QOS_KEM_MLKEM768_SS_BYTES 32u

int pq_kem_mlkem768_available(void);

int pq_kem_mlkem768_keypair(unsigned char* pk, unsigned int pk_len,
                            unsigned char* sk, unsigned int sk_len);

int pq_kem_mlkem768_encaps(unsigned char* ct, unsigned int ct_len,
                           unsigned char* ss, unsigned int ss_len,
                           const unsigned char* pk, unsigned int pk_len);

int pq_kem_mlkem768_decaps(unsigned char* ss, unsigned int ss_len,
                           const unsigned char* ct, unsigned int ct_len,
                           const unsigned char* sk, unsigned int sk_len);

int pq_kem_mlkem768_self_test(void);

#endif
