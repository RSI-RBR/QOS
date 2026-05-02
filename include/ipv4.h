#ifndef IPV4_H
#define IPV4_H

#define IPV4_PROTO_ICMP 1u
#define IPV4_PROTO_TCP  6u
#define IPV4_PROTO_UDP  17u

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
void ipv4_set_local_endpoint(const unsigned char mac[6],
                             const unsigned char ip[4],
                             const unsigned char gateway_ip[4]);
void ipv4_handle_frame(const unsigned char* frame, unsigned int len);
int ipv4_send_via_gateway(unsigned char protocol,
                          const unsigned char dst_ip[4],
                          const unsigned char* payload,
                          unsigned int payload_len);
void ipv4_dump_stats(void);

#endif
