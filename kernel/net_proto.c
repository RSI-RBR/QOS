#include "net_proto.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "uart.h"

typedef struct {
    unsigned long eth_total;
    unsigned long eth_arp;
    unsigned long eth_ipv4;
    unsigned long eth_other;
    unsigned long eth_short;
} net_proto_stats_t;

static net_proto_stats_t g_np_stats;
static const unsigned char g_default_local_mac[6] = {0x02, 0x51, 0x4F, 0x53, 0x00, 0x01};
static const unsigned char g_default_local_ip[4] = {10, 0, 0, 88};
static const unsigned char g_default_gateway_ip[4] = {10, 0, 0, 1};

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
    icmp_init();
}

void net_proto_configure_defaults(void){
    arp_set_local_interface(g_default_local_mac, g_default_local_ip);
    arp_set_periodic_target(g_default_gateway_ip, 2000); // 2s retries until first reply.
    uart_puts("NET defaults: local ip 10.0.0.88, gateway 10.0.0.1\n");
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
    icmp_dump_stats();
}

void net_proto_get_local_mac(unsigned char out_mac[6]){
    if (!out_mac){
        return;
    }
    for (unsigned int i = 0; i < 6; i++){
        out_mac[i] = g_default_local_mac[i];
    }
}

void net_proto_get_local_ip(unsigned char out_ip[4]){
    if (!out_ip){
        return;
    }
    for (unsigned int i = 0; i < 4; i++){
        out_ip[i] = g_default_local_ip[i];
    }
}

void net_proto_get_gateway_ip(unsigned char out_ip[4]){
    if (!out_ip){
        return;
    }
    for (unsigned int i = 0; i < 4; i++){
        out_ip[i] = g_default_gateway_ip[i];
    }
}
