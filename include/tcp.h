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
int tcp_http_get_ex(const unsigned char dst_ip[4],
                    const char* host,
                    const char* path,
                    unsigned char* out,
                    unsigned int out_cap,
                    int accept_gzip);
int tcp_https_get(const unsigned char dst_ip[4],
                  const char* host,
                  const char* path,
                  unsigned char* out,
                  unsigned int out_cap);
int tcp_https_stream_start(const unsigned char dst_ip[4],
                           const char* host,
                           const char* path,
                           unsigned char* out,
                           unsigned int out_cap);
int tcp_https_stream_start_ex(const unsigned char dst_ip[4],
                              const char* host,
                              const char* path,
                              unsigned char* out,
                              unsigned int out_cap,
                              int accept_gzip,
                              int prefer_pq_sig_mldsa65);
int tcp_https_stream_read(unsigned char* out,
                          unsigned int out_cap,
                          unsigned int timeout_ms);
void tcp_https_stream_close(void);
int tcp_https_stream_active(void);
void tcp_dump_stats(void);
int tcp_tls13_pq_sig_offered(void);
int tcp_tls13_pq_kex_offered(void);
int tcp_tls13_pq_kex_active(void);

#endif
