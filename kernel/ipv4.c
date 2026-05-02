#include "ipv4.h"
#include "icmp.h"
#include "uart.h"

typedef struct {
    unsigned long rx_total;
    unsigned long rx_valid;
    unsigned long rx_too_short;
    unsigned long rx_bad_version;
    unsigned long rx_bad_ihl;
} ipv4_stats_t;

static ipv4_stats_t g_ipv4_stats;

static unsigned short be16(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

void ipv4_init(void){
    g_ipv4_stats.rx_total = 0;
    g_ipv4_stats.rx_valid = 0;
    g_ipv4_stats.rx_too_short = 0;
    g_ipv4_stats.rx_bad_version = 0;
    g_ipv4_stats.rx_bad_ihl = 0;
}

void ipv4_handle_frame(const unsigned char* frame, unsigned int len){
    g_ipv4_stats.rx_total++;
    if (!frame || len < sizeof(ipv4_header_t)){
        g_ipv4_stats.rx_too_short++;
        return;
    }

    const ipv4_header_t* ip = (const ipv4_header_t*)frame;
    unsigned int version = (ip->ver_ihl >> 4) & 0xFu;
    unsigned int ihl_words = ip->ver_ihl & 0xFu;
    unsigned int ihl_bytes = ihl_words * 4u;
    if (version != 4u){
        g_ipv4_stats.rx_bad_version++;
        return;
    }
    if (ihl_bytes < 20u || ihl_bytes > len){
        g_ipv4_stats.rx_bad_ihl++;
        return;
    }

    unsigned int total_len = be16((const unsigned char*)&ip->total_len_be);
    if (total_len < ihl_bytes || total_len > len){
        g_ipv4_stats.rx_too_short++;
        return;
    }

    g_ipv4_stats.rx_valid++;
    if (ip->protocol == 1u){
        icmp_handle_ipv4_packet(ip->src, ip->dst, frame + ihl_bytes, total_len - ihl_bytes);
    }
}

void ipv4_dump_stats(void){
    uart_puts("IPv4 rx=");
    uart_putdec(g_ipv4_stats.rx_total);
    uart_puts(" valid=");
    uart_putdec(g_ipv4_stats.rx_valid);
    uart_puts(" short=");
    uart_putdec(g_ipv4_stats.rx_too_short);
    uart_puts(" ver=");
    uart_putdec(g_ipv4_stats.rx_bad_version);
    uart_puts(" ihl=");
    uart_putdec(g_ipv4_stats.rx_bad_ihl);
    uart_puts("\n");
}
