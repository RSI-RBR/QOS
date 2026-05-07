#include "lamport.h"
#include "sha256.h"
#include "crypto.h"

static unsigned int bit_from_digest(const unsigned char digest[32], unsigned int bit_index){
    unsigned int byte_i = bit_index >> 3;
    unsigned int bit_i = 7u - (bit_index & 7u);
    return (unsigned int)((digest[byte_i] >> bit_i) & 1u);
}

int lamport_verify_digest_sha256(const unsigned char message_digest[32],
                                 const unsigned char* signature,
                                 unsigned int signature_len,
                                 const unsigned char* public_key,
                                 unsigned int public_key_len){
    if (!message_digest || !signature || !public_key){
        return -1;
    }
    if (signature_len != LAMPORT_SIG_BYTES || public_key_len != LAMPORT_PUBKEY_BYTES){
        return -1;
    }

    unsigned char h[32];
    for (unsigned int i = 0; i < LAMPORT_BITS; i++){
        const unsigned char* sig_elem = signature + (i * LAMPORT_HASH_BYTES);
        unsigned int b = bit_from_digest(message_digest, i);
        const unsigned char* pub_elem = public_key + (((i * 2u) + b) * LAMPORT_HASH_BYTES);

        sha256_digest(sig_elem, LAMPORT_HASH_BYTES, h);
        if (!crypto_consttime_equal(h, pub_elem, LAMPORT_HASH_BYTES)){
            return -1;
        }
    }
    crypto_memzero(h, sizeof(h));
    return 0;
}

int lamport_self_test(void){
    // Deterministic sanity test: signature equals expected preimage for each selected bit.
    unsigned char digest[32];
    unsigned char signature[LAMPORT_SIG_BYTES];
    unsigned char public_key[LAMPORT_PUBKEY_BYTES];
    unsigned char sk_elem[32];
    unsigned char hash_elem[32];

    for (unsigned int i = 0; i < 32u; i++){
        digest[i] = (unsigned char)(0xA0u + i);
    }

    for (unsigned int i = 0; i < LAMPORT_BITS; i++){
        for (unsigned int b = 0; b < 2u; b++){
            for (unsigned int j = 0; j < 32u; j++){
                sk_elem[j] = (unsigned char)(i + (b * 17u) + j);
            }
            sha256_digest(sk_elem, 32u, hash_elem);
            unsigned char* pk = public_key + (((i * 2u) + b) * 32u);
            for (unsigned int j = 0; j < 32u; j++){
                pk[j] = hash_elem[j];
            }
        }

        unsigned int bit = bit_from_digest(digest, i);
        for (unsigned int j = 0; j < 32u; j++){
            signature[(i * 32u) + j] = (unsigned char)(i + (bit * 17u) + j);
        }
    }

    if (lamport_verify_digest_sha256(digest, signature, sizeof(signature),
                                     public_key, sizeof(public_key)) != 0){
        return -1;
    }

    signature[0] ^= 0x01u;
    if (lamport_verify_digest_sha256(digest, signature, sizeof(signature),
                                     public_key, sizeof(public_key)) == 0){
        return -1;
    }

    return 0;
}
