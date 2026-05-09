#ifndef TRUST_H
#define TRUST_H

#include "program.h"
#include "pq_sig.h"

#define TRUST_ROLE_ADMIN     (1u << 0)
#define TRUST_ROLE_DEVELOPER (1u << 1)

#define TRUST_KEY_ID_ADMIN_MAIN 0x00000001u
#define TRUST_KEY_ID_DEV_MAIN   0x00010001u

#define TRUST_SCOPE_KERNEL    (1u << 0)
#define TRUST_SCOPE_SHELL     (1u << 1)
#define TRUST_SCOPE_WEB       (1u << 2)
#define TRUST_SCOPE_USER_APP  (1u << 3)
#define TRUST_SCOPE_AUTH      (1u << 4)

typedef struct {
    unsigned int key_id;
    const char* name;
    unsigned int role_mask;
    unsigned int scope_mask;
    unsigned int sig_alg_mask;
    unsigned int pq_sig_alg_mask;
    unsigned char ed25519_pubkey[32];
    unsigned int pq_pubkey_len;
    unsigned char pq_pubkey[QOS_PQ_MAX_PUBKEY_BYTES];
    int revoked;
} trust_key_t;

const trust_key_t* trust_find_key(unsigned int key_id);
int trust_verify_program_image(const char* fat_name_83,
                               const program_sec_header_t* sec,
                               const unsigned char* code,
                               unsigned int code_size);

#endif
