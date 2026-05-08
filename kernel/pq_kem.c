#include "pq_kem.h"
#include "crypto.h"

#if defined(QOS_HAVE_PQCLEAN_MLKEM768)
#include "../third_party/pqclean/crypto_kem/ml-kem-768/clean/api.h"
#elif defined(QOS_HAVE_PQCLEAN_KYBER768)
#include "../third_party/pqclean/crypto_kem/kyber768/clean/api.h"
#endif

int pq_kem_mlkem768_available(void){
#if defined(QOS_HAVE_PQCLEAN_MLKEM768)
    return 1;
#elif defined(QOS_HAVE_PQCLEAN_KYBER768)
    return 1;
#else
    return 0;
#endif
}

int pq_kem_mlkem768_keypair(unsigned char* pk, unsigned int pk_len,
                            unsigned char* sk, unsigned int sk_len){
#if defined(QOS_HAVE_PQCLEAN_MLKEM768)
    if (!pk || !sk){
        return -1;
    }
    if (pk_len != QOS_KEM_MLKEM768_PK_BYTES || sk_len != QOS_KEM_MLKEM768_SK_BYTES){
        return -1;
    }
    return PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair(pk, sk);
#elif defined(QOS_HAVE_PQCLEAN_KYBER768)
    if (!pk || !sk){
        return -1;
    }
    if (pk_len != QOS_KEM_MLKEM768_PK_BYTES || sk_len != QOS_KEM_MLKEM768_SK_BYTES){
        return -1;
    }
    return PQCLEAN_KYBER768_CLEAN_crypto_kem_keypair(pk, sk);
#else
    (void)pk; (void)pk_len; (void)sk; (void)sk_len;
    return -1;
#endif
}

int pq_kem_mlkem768_encaps(unsigned char* ct, unsigned int ct_len,
                           unsigned char* ss, unsigned int ss_len,
                           const unsigned char* pk, unsigned int pk_len){
#if defined(QOS_HAVE_PQCLEAN_MLKEM768)
    if (!ct || !ss || !pk){
        return -1;
    }
    if (ct_len != QOS_KEM_MLKEM768_CT_BYTES ||
        ss_len != QOS_KEM_MLKEM768_SS_BYTES ||
        pk_len != QOS_KEM_MLKEM768_PK_BYTES){
        return -1;
    }
    return PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc(ct, ss, pk);
#elif defined(QOS_HAVE_PQCLEAN_KYBER768)
    if (!ct || !ss || !pk){
        return -1;
    }
    if (ct_len != QOS_KEM_MLKEM768_CT_BYTES ||
        ss_len != QOS_KEM_MLKEM768_SS_BYTES ||
        pk_len != QOS_KEM_MLKEM768_PK_BYTES){
        return -1;
    }
    return PQCLEAN_KYBER768_CLEAN_crypto_kem_enc(ct, ss, pk);
#else
    (void)ct; (void)ct_len; (void)ss; (void)ss_len; (void)pk; (void)pk_len;
    return -1;
#endif
}

int pq_kem_mlkem768_decaps(unsigned char* ss, unsigned int ss_len,
                           const unsigned char* ct, unsigned int ct_len,
                           const unsigned char* sk, unsigned int sk_len){
#if defined(QOS_HAVE_PQCLEAN_MLKEM768)
    if (!ss || !ct || !sk){
        return -1;
    }
    if (ss_len != QOS_KEM_MLKEM768_SS_BYTES ||
        ct_len != QOS_KEM_MLKEM768_CT_BYTES ||
        sk_len != QOS_KEM_MLKEM768_SK_BYTES){
        return -1;
    }
    return PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec(ss, ct, sk);
#elif defined(QOS_HAVE_PQCLEAN_KYBER768)
    if (!ss || !ct || !sk){
        return -1;
    }
    if (ss_len != QOS_KEM_MLKEM768_SS_BYTES ||
        ct_len != QOS_KEM_MLKEM768_CT_BYTES ||
        sk_len != QOS_KEM_MLKEM768_SK_BYTES){
        return -1;
    }
    return PQCLEAN_KYBER768_CLEAN_crypto_kem_dec(ss, ct, sk);
#else
    (void)ss; (void)ss_len; (void)ct; (void)ct_len; (void)sk; (void)sk_len;
    return -1;
#endif
}

int pq_kem_mlkem768_self_test(void){
#if defined(QOS_HAVE_PQCLEAN_MLKEM768) || defined(QOS_HAVE_PQCLEAN_KYBER768)
    unsigned char pk[QOS_KEM_MLKEM768_PK_BYTES];
    unsigned char sk[QOS_KEM_MLKEM768_SK_BYTES];
    unsigned char ct[QOS_KEM_MLKEM768_CT_BYTES];
    unsigned char ss_a[QOS_KEM_MLKEM768_SS_BYTES];
    unsigned char ss_b[QOS_KEM_MLKEM768_SS_BYTES];
    int rc = -1;

    if (pq_kem_mlkem768_keypair(pk, sizeof(pk), sk, sizeof(sk)) != 0){
        goto done;
    }
    if (pq_kem_mlkem768_encaps(ct, sizeof(ct), ss_a, sizeof(ss_a), pk, sizeof(pk)) != 0){
        goto done;
    }
    if (pq_kem_mlkem768_decaps(ss_b, sizeof(ss_b), ct, sizeof(ct), sk, sizeof(sk)) != 0){
        goto done;
    }
    rc = crypto_consttime_equal(ss_a, ss_b, QOS_KEM_MLKEM768_SS_BYTES) ? 0 : -1;

done:
    crypto_memzero(pk, sizeof(pk));
    crypto_memzero(sk, sizeof(sk));
    crypto_memzero(ct, sizeof(ct));
    crypto_memzero(ss_a, sizeof(ss_a));
    crypto_memzero(ss_b, sizeof(ss_b));
    return rc;
#else
    return -1;
#endif
}
