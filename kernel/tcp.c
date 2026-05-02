#include "tcp.h"
#include "ipv4.h"
#include "net_proto.h"
#include "timer.h"
#include "net.h"
#include "uart.h"

typedef struct __attribute__((packed)) {
    unsigned short src_port_be;
    unsigned short dst_port_be;
    unsigned int seq_be;
    unsigned int ack_be;
    unsigned char data_off_flags_hi; // upper nibble=data offset
    unsigned char flags_lo;
    unsigned short window_be;
    unsigned short checksum_be;
    unsigned short urg_ptr_be;
} tcp_header_t;

typedef struct {
    unsigned long rx_total;
    unsigned long rx_match;
    unsigned long rx_data;
    unsigned long rx_drop;
    unsigned long tx_total;
    unsigned long tx_fail;
    unsigned long syn_sent;
    unsigned long synack_rx;
    unsigned long est_ok;
    unsigned long fin_rx;
    unsigned long http_ok;
    unsigned long http_fail;
} tcp_stats_t;

enum {
    TCP_ST_CLOSED = 0,
    TCP_ST_SYN_SENT = 1,
    TCP_ST_ESTABLISHED = 2,
    TCP_ST_CLOSE_WAIT = 3
};

typedef struct {
    int active;
    int state;
    unsigned char peer_ip[4];
    unsigned short peer_port;
    unsigned short local_port;
    unsigned int iss;
    unsigned int snd_una;
    unsigned int snd_nxt;
    unsigned int rcv_nxt;
    unsigned char* out;
    unsigned int out_cap;
    unsigned int out_len;
    unsigned long last_rx_tick;
} tcp_conn_t;

static tcp_stats_t g_tcp_stats;
static tcp_conn_t g_conn;
static unsigned short g_next_local_port = 42000u;

#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u

static unsigned short be16_read(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static unsigned long read_cntfrq(void){
    unsigned long v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static unsigned long read_cntpct(void){
    unsigned long v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static void be16_write(unsigned char* p, unsigned short v){
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned int be32_read(const unsigned char* p){
    return ((unsigned int)p[0] << 24) |
           ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) |
           ((unsigned int)p[3]);
}

static void be32_write(unsigned char* p, unsigned int v){
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8) & 0xFFu);
    p[3] = (unsigned char)(v & 0xFFu);
}

static int ip4_eq(const unsigned char a[4], const unsigned char b[4]){
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static unsigned long sum_add_bytes(unsigned long sum, const unsigned char* data, unsigned int len){
    unsigned int i = 0;
    while (i + 1u < len){
        sum += ((unsigned long)data[i] << 8) | data[i + 1u];
        i += 2u;
    }
    if (i < len){
        sum += ((unsigned long)data[i] << 8);
    }
    return sum;
}

static unsigned short sum_finalize(unsigned long sum){
    while (sum >> 16){
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (unsigned short)(~sum & 0xFFFFu);
}

static unsigned short tcp_checksum(const unsigned char src_ip[4],
                                   const unsigned char dst_ip[4],
                                   const unsigned char* tcp_bytes,
                                   unsigned int tcp_len){
    unsigned long sum = 0;
    sum = sum_add_bytes(sum, src_ip, 4);
    sum = sum_add_bytes(sum, dst_ip, 4);
    sum += 6u; // TCP
    sum += (unsigned long)tcp_len;
    sum = sum_add_bytes(sum, tcp_bytes, tcp_len);
    return sum_finalize(sum);
}

static int tcp_send_segment(unsigned char flags,
                            const unsigned char* payload,
                            unsigned int payload_len){
    unsigned char local_ip[4];
    unsigned char buf[20 + 1024];
    unsigned int tcp_len = 20u + payload_len;

    if (!g_conn.active || payload_len > 1024u){
        g_tcp_stats.tx_fail++;
        return -1;
    }

    for (unsigned int i = 0; i < tcp_len; i++){
        buf[i] = 0;
    }
    be16_write(&buf[0], g_conn.local_port);
    be16_write(&buf[2], g_conn.peer_port);
    be32_write(&buf[4], g_conn.snd_nxt);
    be32_write(&buf[8], g_conn.rcv_nxt);
    buf[12] = (unsigned char)(5u << 4); // data offset
    buf[13] = flags;
    be16_write(&buf[14], 4096u);
    be16_write(&buf[16], 0);
    be16_write(&buf[18], 0);

    for (unsigned int i = 0; i < payload_len; i++){
        buf[20 + i] = payload[i];
    }

    net_proto_get_local_ip(local_ip);
    unsigned short csum = tcp_checksum(local_ip, g_conn.peer_ip, buf, tcp_len);
    if (csum == 0){
        csum = 0xFFFFu;
    }
    be16_write(&buf[16], csum);

    if (ipv4_send_via_gateway(6u, g_conn.peer_ip, buf, tcp_len) != 0){
        g_tcp_stats.tx_fail++;
        return -1;
    }
    g_tcp_stats.tx_total++;

    if (flags & TCP_FLAG_SYN){
        g_conn.snd_nxt += 1u;
    }
    if (flags & TCP_FLAG_FIN){
        g_conn.snd_nxt += 1u;
    }
    g_conn.snd_nxt += payload_len;
    return 0;
}

void tcp_init(void){
    g_tcp_stats.rx_total = 0;
    g_tcp_stats.rx_match = 0;
    g_tcp_stats.rx_data = 0;
    g_tcp_stats.rx_drop = 0;
    g_tcp_stats.tx_total = 0;
    g_tcp_stats.tx_fail = 0;
    g_tcp_stats.syn_sent = 0;
    g_tcp_stats.synack_rx = 0;
    g_tcp_stats.est_ok = 0;
    g_tcp_stats.fin_rx = 0;
    g_tcp_stats.http_ok = 0;
    g_tcp_stats.http_fail = 0;
    g_conn.active = 0;
    g_conn.state = TCP_ST_CLOSED;
}

void tcp_handle_ipv4_packet(const unsigned char src_ip[4],
                            const unsigned char dst_ip[4],
                            const unsigned char* payload,
                            unsigned int payload_len){
    (void)dst_ip;
    g_tcp_stats.rx_total++;
    if (!g_conn.active || !src_ip || !payload || payload_len < 20u){
        g_tcp_stats.rx_drop++;
        return;
    }

    const tcp_header_t* h = (const tcp_header_t*)payload;
    unsigned short src_port = be16_read((const unsigned char*)&h->src_port_be);
    unsigned short dst_port = be16_read((const unsigned char*)&h->dst_port_be);
    unsigned int seq = be32_read((const unsigned char*)&h->seq_be);
    unsigned int ack = be32_read((const unsigned char*)&h->ack_be);
    unsigned int hdr_len = ((unsigned int)(h->data_off_flags_hi >> 4) & 0xFu) * 4u;
    unsigned char flags = h->flags_lo;

    if (hdr_len < 20u || hdr_len > payload_len){
        g_tcp_stats.rx_drop++;
        return;
    }
    if (!ip4_eq(src_ip, g_conn.peer_ip) || src_port != g_conn.peer_port || dst_port != g_conn.local_port){
        g_tcp_stats.rx_drop++;
        return;
    }
    g_tcp_stats.rx_match++;

    if (flags & TCP_FLAG_RST){
        g_conn.state = TCP_ST_CLOSED;
        g_conn.active = 0;
        return;
    }

    if (g_conn.state == TCP_ST_SYN_SENT){
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == g_conn.snd_nxt){
            g_tcp_stats.synack_rx++;
            g_conn.snd_una = ack;
            g_conn.rcv_nxt = seq + 1u;
            if (tcp_send_segment(TCP_FLAG_ACK, 0, 0) == 0){
                g_conn.state = TCP_ST_ESTABLISHED;
                g_tcp_stats.est_ok++;
            }
        }
        return;
    }

    if ((flags & TCP_FLAG_ACK) && ack > g_conn.snd_una && ack <= g_conn.snd_nxt){
        g_conn.snd_una = ack;
    }

    unsigned int data_len = payload_len - hdr_len;
    if (data_len > 0){
        if (seq == g_conn.rcv_nxt){
            unsigned int room = (g_conn.out_len < g_conn.out_cap) ? (g_conn.out_cap - g_conn.out_len) : 0u;
            unsigned int take = (data_len < room) ? data_len : room;
            for (unsigned int i = 0; i < take; i++){
                g_conn.out[g_conn.out_len + i] = payload[hdr_len + i];
            }
            g_conn.out_len += take;
            g_conn.rcv_nxt += data_len;
            g_conn.last_rx_tick = system_ticks;
            g_tcp_stats.rx_data += data_len;
            (void)tcp_send_segment(TCP_FLAG_ACK, 0, 0);
        } else{
            (void)tcp_send_segment(TCP_FLAG_ACK, 0, 0);
        }
    }

    if (flags & TCP_FLAG_FIN){
        g_tcp_stats.fin_rx++;
        if (seq + data_len == g_conn.rcv_nxt){
            g_conn.rcv_nxt += 1u;
        } else{
            g_conn.rcv_nxt = seq + data_len + 1u;
        }
        (void)tcp_send_segment(TCP_FLAG_ACK, 0, 0);
        g_conn.state = TCP_ST_CLOSE_WAIT;
        g_conn.last_rx_tick = system_ticks;
    }
}

static int append_str(char* dst, int cap, int* idx, const char* s){
    while (*s){
        if (*idx >= cap){
            return -1;
        }
        dst[*idx] = *s;
        (*idx)++;
        s++;
    }
    return 0;
}

int tcp_http_get(const unsigned char dst_ip[4],
                 const char* host,
                 const char* path,
                 unsigned char* out,
                 unsigned int out_cap){
    char req[512];
    int rq = 0;
    unsigned long start_tick;
    unsigned long last_progress;
    unsigned long freq;
    unsigned long start_cnt;
    unsigned long last_progress_cnt;
    unsigned long handshake_to_cnt;
    unsigned long overall_to_cnt;
    unsigned long idle_to_cnt;
    unsigned long spin_budget;
    const char* req_path = path && *path ? path : "/";

    if (!dst_ip || !host || !*host || !out || out_cap == 0){
        g_tcp_stats.http_fail++;
        return -1;
    }
    if (!net_ready() || !net_link_up()){
        g_tcp_stats.http_fail++;
        return -1;
    }

    if (g_conn.active){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
    }

    for (unsigned int i = 0; i < 4; i++){
        g_conn.peer_ip[i] = dst_ip[i];
    }
    g_conn.peer_port = 80u;
    g_conn.local_port = g_next_local_port++;
    if (g_next_local_port < 42000u || g_next_local_port > 52000u){
        g_next_local_port = 42000u;
    }
    g_conn.iss = (unsigned int)(system_ticks * 1103515245u + 12345u);
    g_conn.snd_una = g_conn.iss;
    g_conn.snd_nxt = g_conn.iss;
    g_conn.rcv_nxt = 0u;
    g_conn.out = out;
    g_conn.out_cap = out_cap - 1u; // reserve null terminator slot
    g_conn.out_len = 0u;
    g_conn.last_rx_tick = system_ticks;
    g_conn.state = TCP_ST_SYN_SENT;
    g_conn.active = 1;

    if (tcp_send_segment(TCP_FLAG_SYN, 0, 0) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }
    g_tcp_stats.syn_sent++;

    freq = read_cntfrq();
    if (freq == 0){
        freq = 1000000UL;
    }
    handshake_to_cnt = (freq / 1000UL) * 2500UL; // 2.5s
    overall_to_cnt = (freq / 1000UL) * 9000UL;   // 9s
    idle_to_cnt = (freq / 1000UL) * 1500UL;      // 1.5s
    if (handshake_to_cnt == 0) handshake_to_cnt = freq;
    if (overall_to_cnt == 0) overall_to_cnt = freq * 2UL;
    if (idle_to_cnt == 0) idle_to_cnt = freq / 2UL;

    start_tick = system_ticks;
    start_cnt = read_cntpct();
    spin_budget = 25000000UL;
    while (g_conn.state == TCP_ST_SYN_SENT){
        (void)net_poll();
        if ((read_cntpct() - start_cnt) > handshake_to_cnt){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            return -1;
        }
        if ((long)(system_ticks - start_tick) > 2000){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            return -1;
        }
        if (spin_budget-- == 0){
            g_conn.active = 0;
            g_conn.state = TCP_ST_CLOSED;
            g_tcp_stats.http_fail++;
            return -1;
        }
    }
    if (g_conn.state != TCP_ST_ESTABLISHED){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }

    if (append_str(req, (int)sizeof(req), &rq, "GET ") != 0 ||
        append_str(req, (int)sizeof(req), &rq, req_path) != 0 ||
        append_str(req, (int)sizeof(req), &rq, " HTTP/1.1\r\nHost: ") != 0 ||
        append_str(req, (int)sizeof(req), &rq, host) != 0 ||
        append_str(req, (int)sizeof(req), &rq, "\r\nUser-Agent: QOS/0.1\r\nConnection: close\r\n\r\n") != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }

    if (tcp_send_segment((unsigned char)(TCP_FLAG_ACK | TCP_FLAG_PSH), (const unsigned char*)req, (unsigned int)rq) != 0){
        g_conn.active = 0;
        g_conn.state = TCP_ST_CLOSED;
        g_tcp_stats.http_fail++;
        return -1;
    }

    start_tick = system_ticks;
    last_progress = system_ticks;
    start_cnt = read_cntpct();
    last_progress_cnt = start_cnt;
    unsigned int prev_len = 0u;
    spin_budget = 80000000UL;
    while (1){
        (void)net_poll();
        if (g_conn.out_len != prev_len){
            prev_len = g_conn.out_len;
            last_progress = system_ticks;
            last_progress_cnt = read_cntpct();
        }
        if (g_conn.state == TCP_ST_CLOSE_WAIT){
            break;
        }
        if (g_conn.out_len >= g_conn.out_cap){
            break;
        }
        if ((read_cntpct() - start_cnt) > overall_to_cnt){
            break;
        }
        if ((read_cntpct() - last_progress_cnt) > idle_to_cnt && g_conn.out_len > 0){
            break;
        }
        if ((long)(system_ticks - last_progress) > 1200 && g_conn.out_len > 0){
            break;
        }
        if ((long)(system_ticks - start_tick) > 7000){
            break;
        }
        if (spin_budget-- == 0){
            break;
        }
    }

    g_conn.active = 0;
    g_conn.state = TCP_ST_CLOSED;
    out[g_conn.out_len] = 0;
    if (g_conn.out_len > 0u){
        g_tcp_stats.http_ok++;
        return (int)g_conn.out_len;
    }
    g_tcp_stats.http_fail++;
    return -1;
}

void tcp_dump_stats(void){
    uart_puts("TCP rx=");
    uart_putdec(g_tcp_stats.rx_total);
    uart_puts(" match=");
    uart_putdec(g_tcp_stats.rx_match);
    uart_puts(" rx_data=");
    uart_putdec(g_tcp_stats.rx_data);
    uart_puts(" rx_drop=");
    uart_putdec(g_tcp_stats.rx_drop);
    uart_puts(" tx=");
    uart_putdec(g_tcp_stats.tx_total);
    uart_puts(" tx_fail=");
    uart_putdec(g_tcp_stats.tx_fail);
    uart_puts(" syn=");
    uart_putdec(g_tcp_stats.syn_sent);
    uart_puts(" synack=");
    uart_putdec(g_tcp_stats.synack_rx);
    uart_puts(" est=");
    uart_putdec(g_tcp_stats.est_ok);
    uart_puts(" fin=");
    uart_putdec(g_tcp_stats.fin_rx);
    uart_puts(" http_ok=");
    uart_putdec(g_tcp_stats.http_ok);
    uart_puts(" http_fail=");
    uart_putdec(g_tcp_stats.http_fail);
    uart_puts("\n");
}
