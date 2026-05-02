#include "arp.h"
#include "uart.h"

typedef struct {
    unsigned long rx_total;
    unsigned long rx_valid;
    unsigned long rx_request;
    unsigned long rx_reply;
    unsigned long rx_unsupported;
} arp_stats_t;

static arp_stats_t g_arp_stats;

static unsigned short be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

void arp_init(void){
    g_arp_stats.rx_total = 0;
    g_arp_stats.rx_valid = 0;
    g_arp_stats.rx_request = 0;
    g_arp_stats.rx_reply = 0;
    g_arp_stats.rx_unsupported = 0;
}

void arp_handle_frame(const unsigned char* frame, unsigned int len){
    g_arp_stats.rx_total++;
    if (!frame || len < sizeof(arp_packet_t)){
        return;
    }

    const arp_packet_t* arp = (const arp_packet_t*)frame;
    unsigned short htype = be16((const unsigned char*)&arp->htype_be);
    unsigned short ptype = be16((const unsigned char*)&arp->ptype_be);
    unsigned short oper = be16((const unsigned char*)&arp->oper_be);

    if (htype != ARP_HTYPE_ETHERNET ||
        ptype != ARP_PTYPE_IPV4 ||
        arp->hlen != ARP_HLEN_ETHERNET ||
        arp->plen != ARP_PLEN_IPV4){
        g_arp_stats.rx_unsupported++;
        return;
    }

    g_arp_stats.rx_valid++;
    if (oper == ARP_OP_REQUEST){
        g_arp_stats.rx_request++;
        // Scaffold hook: respond if target IP matches local IP.
    } else if (oper == ARP_OP_REPLY){
        g_arp_stats.rx_reply++;
        // Scaffold hook: learn sender IP -> MAC in ARP cache.
    } else{
        g_arp_stats.rx_unsupported++;
    }
}

void arp_dump_stats(void){
    uart_puts("ARP rx=");
    uart_puthex((unsigned int)g_arp_stats.rx_total);
    uart_puts(" valid=");
    uart_puthex((unsigned int)g_arp_stats.rx_valid);
    uart_puts(" req=");
    uart_puthex((unsigned int)g_arp_stats.rx_request);
    uart_puts(" rep=");
    uart_puthex((unsigned int)g_arp_stats.rx_reply);
    uart_puts(" bad=");
    uart_puthex((unsigned int)g_arp_stats.rx_unsupported);
    uart_puts("\n");
}
