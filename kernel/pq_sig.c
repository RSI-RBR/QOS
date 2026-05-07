#include "pq_sig.h"
#include "crypto.h"
#include "sign.h"
#include "uart.h"

static int g_warned_missing_mldsa_backend = 0;

const char* pq_sig_alg_name(unsigned int sig_alg){
    if (sig_alg == QOS_SIG_ALG_MLDSA65){
        return "ML-DSA-65";
    }
    return "unknown";
}

unsigned int pq_sig_pubkey_expected_len(unsigned int sig_alg){
    if (sig_alg == QOS_SIG_ALG_MLDSA65){
        return QOS_PQ_MLDSA65_PUBKEY_BYTES;
    }
    return 0u;
}

unsigned int pq_sig_signature_expected_len(unsigned int sig_alg){
    if (sig_alg == QOS_SIG_ALG_MLDSA65){
        return QOS_PQ_MLDSA65_SIG_BYTES;
    }
    return 0u;
}

int pq_sig_verify_digest_sha256(unsigned int sig_alg,
                                const unsigned char digest[32],
                                const unsigned char* signature,
                                unsigned int signature_len,
                                const unsigned char* public_key,
                                unsigned int public_key_len){
    if (!digest || !signature || !public_key){
        return -1;
    }

    if (sig_alg != QOS_SIG_ALG_MLDSA65){
        return -1;
    }

    if (public_key_len != QOS_PQ_MLDSA65_PUBKEY_BYTES ||
        signature_len != QOS_PQ_MLDSA65_SIG_BYTES){
        return -1;
    }

    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify(signature, (size_t)signature_len,
                                                  digest, 32u, public_key) != 0){
        return -1;
    }
    return 0;
}

int pq_sig_self_test(void){
    // Negative-path sanity only (no embedded test vectors here).
    unsigned char digest[32];
    unsigned char sig[QOS_PQ_MLDSA65_SIG_BYTES];
    unsigned char pk[QOS_PQ_MLDSA65_PUBKEY_BYTES];
    crypto_memzero(digest, sizeof(digest));
    crypto_memzero(sig, sizeof(sig));
    crypto_memzero(pk, sizeof(pk));

    if (pq_sig_verify_digest_sha256(QOS_SIG_ALG_MLDSA65, digest, sig, sizeof(sig),
                                    pk, sizeof(pk)) == 0){
        if (!g_warned_missing_mldsa_backend){
            uart_puts("PQ verify: ML-DSA self-test unexpectedly accepted zero vector.\n");
            g_warned_missing_mldsa_backend = 1;
        }
        return -1;
    }
    return 0;
}
