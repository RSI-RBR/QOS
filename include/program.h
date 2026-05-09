#ifndef PROGRAM_H
#define PROGRAM_H

#define QOS_MAGIC 0x514F5350 // "QOSP"
#define QOS_SEC_MAGIC 0x53454331 // "SEC1"
#define QOS_PQ_SIG_MAGIC 0x51505331u // "QPS1"
#define QOS_PQ_SIG_VERSION 1u
#define QOS_PROG_FLAG_SHA256 0x00000001u
#define QOS_PROG_FLAG_MEM_LAYOUT_V1 0x00000002u
#define QOS_PROG_FLAG_RELOC_RELATIVE_V1 0x00000004u
#define QOS_SIG_ALG_DIGEST_ONLY 1u
#define QOS_SIG_ALG_ED25519 2u
#define QOS_SIG_ALG_MLDSA65 3u
// Backward-compat alias for old builds/scripts. New code should use QOS_SIG_ALG_MLDSA65.
#define QOS_SIG_ALG_LAMPORT_SHA256 QOS_SIG_ALG_MLDSA65
#define QOS_MAX_SIGNATURE_BYTES 64u
#define QOS_PROGRAM_POOL_START 0x08000000UL
#define QOS_PROGRAM_POOL_SIZE (128UL * 1024UL * 1024UL)
#define QOS_PROGRAM_ALLOC_GRANULE_BYTES (2UL * 1024UL * 1024UL)
#define QOS_PROGRAM_MAX_MEMORY_BYTES (16UL * 1024UL * 1024UL)
#define QOS_PROGRAM_DEFAULT_MEMORY_BYTES QOS_PROGRAM_MAX_MEMORY_BYTES
#define QOS_PROGRAM_SEC_MAX_HEADER_BYTES 65536u
#define QOS_PROGRAM_MAX_RELOCS 4096u
#define QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES 12u
#define QOS_PROGRAM_SIG_MSG_MAX (16u + 11u + 28u + 32u + 8u + (QOS_PROGRAM_MAX_RELOCS * QOS_PROGRAM_RELOC_RELATIVE_ENTRY_BYTES))
// Backward-compat alias for code that still uses "slot" to mean one 2 MiB MMU chunk.
#define QOS_PROGRAM_SLOT_SIZE QOS_PROGRAM_ALLOC_GRANULE_BYTES
#ifndef QOS_PROGRAM_MEMORY_BYTES
#define QOS_PROGRAM_MEMORY_BYTES QOS_PROGRAM_DEFAULT_MEMORY_BYTES
#endif
#define QOS_USER_GUARD_PAGE_BYTES 4096UL
#define QOS_USER_STACK_BYTES (128UL * 1024UL)

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

typedef struct{
    unsigned int user_rw_offset;
    unsigned int user_rw_size;
} program_sec_layout_v1_t;

typedef struct{
    unsigned int reloc_count;
    unsigned int reloc_entry_size;
} program_sec_reloc_v1_t;

#endif
