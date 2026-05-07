#ifndef LAMPORT_H
#define LAMPORT_H

#define LAMPORT_HASH_BYTES 32u
#define LAMPORT_BITS 256u
#define LAMPORT_SIG_BYTES (LAMPORT_BITS * LAMPORT_HASH_BYTES)
#define LAMPORT_PUBKEY_BYTES (LAMPORT_BITS * 2u * LAMPORT_HASH_BYTES)

// Verifies a Lamport one-time signature over a 32-byte message digest.
// Returns 0 on success, -1 on failure.
int lamport_verify_digest_sha256(const unsigned char message_digest[32],
                                 const unsigned char* signature,
                                 unsigned int signature_len,
                                 const unsigned char* public_key,
                                 unsigned int public_key_len);

int lamport_self_test(void);

#endif
