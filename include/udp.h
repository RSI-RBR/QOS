#ifndef UDP_H
#define UDP_H

#define UDP_MAX_PAYLOAD 1472u

typedef struct {
    unsigned char src_ip[4];
    unsigned short src_port;
    unsigned short dst_port;
    unsigned short len;
} udp_meta_t;

void udp_init(void);
void udp_handle_ipv4_packet(const unsigned char src_ip[4],
                            const unsigned char dst_ip[4],
                            const unsigned char* payload,
                            unsigned int payload_len);
int udp_send(const unsigned char dst_ip[4],
             unsigned short src_port,
             unsigned short dst_port,
             const unsigned char* data,
             unsigned int len);
int udp_send_probe_gateway(void);
int udp_recv_next(unsigned char* out, unsigned int out_cap, udp_meta_t* meta);
int udp_recv_filtered(unsigned short dst_port,
                      int require_src,
                      const unsigned char src_ip[4],
                      unsigned short src_port,
                      unsigned char* out,
                      unsigned int out_cap,
                      udp_meta_t* meta);
void udp_dump_stats(void);

#endif
