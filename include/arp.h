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
void arp_set_local_interface(const unsigned char mac[ETH_ADDR_LEN], const unsigned char ip[4]);
int arp_send_request(const unsigned char target_ip[4]);
void arp_set_periodic_target(const unsigned char target_ip[4], unsigned int interval_ms);
void arp_periodic_tick(unsigned long now_ticks);
int arp_gateway_resolved(void);
int arp_get_gateway_mac(unsigned char out_mac[ETH_ADDR_LEN]);
int arp_resolve_gateway(unsigned int timeout_ms);

#endif
