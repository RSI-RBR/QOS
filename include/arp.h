#ifndef ARP_H
#define ARP_H

#include "ethernet.h"

#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4     0x0800u
#define ARP_HLEN_ETHERNET  6u
#define ARP_PLEN_IPV4      4u

#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY   2u

typedef struct __attribute__((packed)) {
    unsigned short htype_be;
    unsigned short ptype_be;
    unsigned char hlen;
    unsigned char plen;
    unsigned short oper_be;
    unsigned char sha[ETH_ADDR_LEN];
    unsigned char spa[4];
    unsigned char tha[ETH_ADDR_LEN];
    unsigned char tpa[4];
} arp_packet_t;

void arp_init(void);
void arp_handle_frame(const unsigned char* frame, unsigned int len);
void arp_dump_stats(void);

#endif
