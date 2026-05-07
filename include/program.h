#ifndef PROGRAM_H
#define PROGRAM_H

#define QOS_MAGIC 0x514F5350 // "QOSP"
#define QOS_SEC_MAGIC 0x53454331 // "SEC1"
#define QOS_PQ_SIG_MAGIC 0x51505331u // "QPS1"
#define QOS_PQ_SIG_VERSION 1u
#define QOS_PROG_FLAG_SHA256 0x00000001u
#define QOS_SIG_ALG_DIGEST_ONLY 1u
#define QOS_SIG_ALG_ED25519 2u
#define QOS_SIG_ALG_LAMPORT_SHA256 3u
#define QOS_MAX_SIGNATURE_BYTES 64u

typedef struct{
    unsigned int magic;
    unsigned int size;
    unsigned int entry_offset;
} program_header_t;

typedef struct{
    unsigned int magic;       // QOS_SEC_MAGIC
    unsigned int header_size; // total bytes of this extension header
    unsigned int flags;       // QOS_PROG_FLAG_*
    unsigned int signer_key_id;
    unsigned int sig_alg;
    unsigned int sig_len;
    unsigned char sha256[32]; // SHA-256 over code payload bytes only
    unsigned char signature[QOS_MAX_SIGNATURE_BYTES];
} program_sec_header_t;

#endif
