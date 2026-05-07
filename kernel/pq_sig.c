#include "pq_sig.h"
#include "crypto.h"
#include "lamport.h"
#include "uart.h"

static int g_warned_legacy_lamport = 0;
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

    // Compatibility bridge:
    // Existing deployments may still carry legacy Lamport key/signature material.
    // This path keeps current signed images bootable while ML-DSA verification
    // backend is being integrated.
    if (public_key_len == LAMPORT_PUBKEY_BYTES && signature_len == LAMPORT_SIG_BYTES){
        if (!g_warned_legacy_lamport){
            uart_puts("PQ verify: legacy Lamport compatibility mode active.\n");
            g_warned_legacy_lamport = 1;
        }
        return lamport_verify_digest_sha256(digest, signature, signature_len,
                                            public_key, public_key_len);
    }

    // Real ML-DSA backend not linked yet.
    if (!g_warned_missing_mldsa_backend){
        uart_puts("PQ verify: ML-DSA backend not linked.\n");
        g_warned_missing_mldsa_backend = 1;
    }
    return -1;
}

int pq_sig_self_test(void){
    // Keep deterministic baseline coverage through compatibility bridge.
    if (lamport_self_test() != 0){
        return -1;
    }
    return 0;
}
