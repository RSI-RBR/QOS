#ifndef ICMP_H
#define ICMP_H

void icmp_init(void);
void icmp_handle_ipv4_packet(const unsigned char* src_ip,
                             const unsigned char* dst_ip,
                             const unsigned char* payload,
                             unsigned int payload_len);
void icmp_dump_stats(void);
int icmp_ping_gateway(unsigned int timeout_ms);

#endif
