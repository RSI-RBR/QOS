#ifndef TCP_H
#define TCP_H

void tcp_init(void);
void tcp_handle_ipv4_packet(const unsigned char src_ip[4],
                            const unsigned char dst_ip[4],
                            const unsigned char* payload,
                            unsigned int payload_len);
int tcp_http_get(const unsigned char dst_ip[4],
                 const char* host,
                 const char* path,
                 unsigned char* out,
                 unsigned int out_cap);
void tcp_dump_stats(void);

#endif
