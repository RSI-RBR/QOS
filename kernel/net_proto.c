#include "net_proto.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "uart.h"

typedef struct {
    unsigned long eth_total;
    unsigned long eth_arp;
    unsigned long eth_ipv4;
    unsigned long eth_other;
    unsigned long eth_short;
} net_proto_stats_t;

static net_proto_stats_t g_np_stats;

static unsigned short be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

void net_proto_init(void){
    g_np_stats.eth_total = 0;
    g_np_stats.eth_arp = 0;
    g_np_stats.eth_ipv4 = 0;
    g_np_stats.eth_other = 0;
    g_np_stats.eth_short = 0;
    arp_init();
    ipv4_init();
}

void net_proto_handle_frame(const unsigned char* frame, unsigned int len){
    g_np_stats.eth_total++;
    if (!frame || len < ETH_HEADER_LEN){
        g_np_stats.eth_short++;
        return;
    }

    const eth_header_t* eth = (const eth_header_t*)frame;
    unsigned short ethertype = be16((const unsigned char*)&eth->ethertype_be);
    const unsigned char* payload = frame + ETH_HEADER_LEN;
    unsigned int payload_len = len - ETH_HEADER_LEN;

    if (ethertype == ETH_TYPE_ARP){
        g_np_stats.eth_arp++;
        arp_handle_frame(payload, payload_len);
        return;
    }
    if (ethertype == ETH_TYPE_IPV4){
        g_np_stats.eth_ipv4++;
        ipv4_handle_frame(payload, payload_len);
        return;
    }

    g_np_stats.eth_other++;
}

void net_proto_dump_stats(void){
    uart_puts("L2 rx=");
    uart_puthex((unsigned int)g_np_stats.eth_total);
    uart_puts(" arp=");
    uart_puthex((unsigned int)g_np_stats.eth_arp);
    uart_puts(" ip=");
    uart_puthex((unsigned int)g_np_stats.eth_ipv4);
    uart_puts(" other=");
    uart_puthex((unsigned int)g_np_stats.eth_other);
    uart_puts(" short=");
    uart_puthex((unsigned int)g_np_stats.eth_short);
    uart_puts("\n");
    arp_dump_stats();
    ipv4_dump_stats();
}
