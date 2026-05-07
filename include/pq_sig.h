#ifndef PQ_SIG_H
#define PQ_SIG_H

#include "program.h"

// FIPS 204 ML-DSA (Dilithium) fixed sizes.
#define QOS_PQ_MLDSA44_PUBKEY_BYTES 1312u
#define QOS_PQ_MLDSA65_PUBKEY_BYTES 1952u
#define QOS_PQ_MLDSA87_PUBKEY_BYTES 2592u

#define QOS_PQ_MLDSA44_SIG_BYTES 2420u
#define QOS_PQ_MLDSA65_SIG_BYTES 3309u
#define QOS_PQ_MLDSA87_SIG_BYTES 4627u

// Legacy bridge sizes (old Lamport path).
#define QOS_PQ_LEGACY_LAMPORT_PUBKEY_BYTES (256u * 2u * 32u)
#define QOS_PQ_LEGACY_LAMPORT_SIG_BYTES (256u * 32u)

#define QOS_PQ_MAX_PUBKEY_BYTES QOS_PQ_LEGACY_LAMPORT_PUBKEY_BYTES
#define QOS_PQ_MAX_SIG_BYTES QOS_PQ_LEGACY_LAMPORT_SIG_BYTES

#define QOS_PQ_SIG_HEADER_BYTES 20u
#define QOS_PQ_SIG_MAX (QOS_PQ_SIG_HEADER_BYTES + QOS_PQ_MAX_SIG_BYTES)

const char* pq_sig_alg_name(unsigned int sig_alg);
unsigned int pq_sig_pubkey_expected_len(unsigned int sig_alg);
unsigned int pq_sig_signature_expected_len(unsigned int sig_alg);

// Verify PQ sidecar signature over a 32-byte digest of the canonical message payload.
// Returns 0 on success, -1 on failure/unsupported.
int pq_sig_verify_digest_sha256(unsigned int sig_alg,
                                const unsigned char digest[32],
                                const unsigned char* signature,
                                unsigned int signature_len,
                                const unsigned char* public_key,
                                unsigned int public_key_len);

int pq_sig_self_test(void);

#endif
