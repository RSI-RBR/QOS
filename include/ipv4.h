#ifndef IPV4_H
#define IPV4_H

typedef struct __attribute__((packed)) {
    unsigned char ver_ihl;
    unsigned char dscp_ecn;
    unsigned short total_len_be;
    unsigned short id_be;
    unsigned short flags_frag_be;
    unsigned char ttl;
    unsigned char protocol;
    unsigned short hdr_checksum_be;
    unsigned char src[4];
    unsigned char dst[4];
} ipv4_header_t;

void ipv4_init(void);
void ipv4_handle_frame(const unsigned char* frame, unsigned int len);
void ipv4_dump_stats(void);

#endif
