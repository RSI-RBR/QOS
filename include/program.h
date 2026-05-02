#ifndef PROGRAM_H
#define PROGRAM_H

#define QOS_MAGIC 0x514F5350 // "QOSP"
#define QOS_SEC_MAGIC 0x53454331 // "SEC1"
#define QOS_PROG_FLAG_SHA256 0x00000001u

typedef struct{
    unsigned int magic;
    unsigned int size;
    unsigned int entry_offset;
} program_header_t;

typedef struct{
    unsigned int magic;       // QOS_SEC_MAGIC
    unsigned int header_size; // total bytes of this extension header
    unsigned int flags;       // QOS_PROG_FLAG_*
    unsigned char sha256[32]; // SHA-256 over code payload bytes only
} program_sec_header_t;

#endif
