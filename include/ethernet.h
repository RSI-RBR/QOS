#ifndef ETHERNET_H
#define ETHERNET_H

#define ETH_ADDR_LEN 6
#define ETH_HEADER_LEN 14
#define ETH_MIN_FRAME_LEN 60
#define ETH_MAX_FRAME_LEN 1518

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806


typedef struct __attribute__((packed)) {
    unsigned char dst[ETH_ADDR_LEN];
    unsigned char src[ETH_ADDR_LEN];
    unsigned short ethertype_be;
} eth_header_t;

static inline unsigned short eth_htons(unsigned short v){
    return (unsigned short)((v << 8) | (v >> 8));
}

#endif
