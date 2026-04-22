#ifndef PROGRAM_H
#define PROGRAM_H

#define QOS_MAGIC 0x514F5350 // "QOSP"

typedef struct{
    unsigned int magic;
    unsigned int size;
    unsigned int entry_offset;
} program_header_t;

#endif
