#ifndef USER_NET_H
#define USER_NET_H

#include "syscall.h"

#define QOS_DNS_ERR_PARAM   (-1)
#define QOS_DNS_ERR_SOCKET  (-2)
#define QOS_DNS_ERR_CONNECT (-3)
#define QOS_DNS_ERR_SEND    (-4)
#define QOS_DNS_ERR_TIMEOUT (-5)
#define QOS_DNS_ERR_PARSE   (-6)
#define QOS_DNS_ERR_NO_A    (-7)

static inline unsigned short qos_be16_read(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static inline int qos_dns_skip_name(const unsigned char* msg, int len, int off){
    if (!msg || len <= 0 || off < 0 || off >= len){
        return -1;
    }
    while (off < len){
        unsigned char c = msg[off];
        if (c == 0){
            return off + 1;
        }
        if ((c & 0xC0u) == 0xC0u){
            if (off + 1 >= len){
                return -1;
            }
            return off + 2;
        }
        if (c > 63u){
            return -1;
        }
        off++;
        if (off + (int)c > len){
            return -1;
        }
        off += (int)c;
    }
    return -1;
}

static inline int qos_dns_build_qname(const char* host, unsigned char* out, int cap){
    int w = 0;
    int label_len = 0;
    int label_pos = -1;
    const char* p = host;

    if (!host || !out || cap <= 0){
        return -1;
    }

    label_pos = w++;
    if (w >= cap){
        return -1;
    }

    while (*p){
        char ch = *p++;
        if (ch == '.'){
            if (label_len <= 0 || label_len > 63){
                return -1;
            }
            out[label_pos] = (unsigned char)label_len;
            label_len = 0;
            label_pos = w++;
            if (w >= cap){
                return -1;
            }
            continue;
        }
        if (label_len >= 63 || w >= cap){
            return -1;
        }
        out[w++] = (unsigned char)ch;
        label_len++;
    }
    if (label_len <= 0 || label_len > 63 || w >= cap){
        return -1;
    }
    out[label_pos] = (unsigned char)label_len;
    out[w++] = 0;
    return w;
}

static inline int qos_dns_resolve_a_socket(const char* host,
                                           const unsigned char dns_server_ip[4],
                                           unsigned char out_ip[4],
                                           unsigned int timeout_ms){
    static unsigned short dns_id = 0x6153u;
    qos_sockaddr_in_t sa;
    unsigned char q[320];
    unsigned char r[600];
    unsigned int qi = 0;
    int qname_len;
    int fd;
    int n;
    const unsigned int slice_ms = 200u;
    unsigned int recv_slice_ms = slice_ms;

    if (!host || !*host || !dns_server_ip || !out_ip){
        return QOS_DNS_ERR_PARAM;
    }

    qname_len = qos_dns_build_qname(host, &q[12], (int)(sizeof(q) - 16));
    if (qname_len <= 0){
        return QOS_DNS_ERR_PARAM;
    }

    fd = qos_socket(QOS_AF_INET, QOS_SOCK_DGRAM, 0);
    if (fd < 0){
        return QOS_DNS_ERR_SOCKET;
    }

    sa.family = QOS_AF_INET;
    sa.port = 53u;
    sa.addr[0] = dns_server_ip[0];
    sa.addr[1] = dns_server_ip[1];
    sa.addr[2] = dns_server_ip[2];
    sa.addr[3] = dns_server_ip[3];
    for (int i = 0; i < (int)sizeof(sa.reserved); i++){
        sa.reserved[i] = 0;
    }

    if (qos_connect(fd, &sa, (unsigned int)sizeof(sa)) != 0){
        (void)qos_close(fd);
        return QOS_DNS_ERR_CONNECT;
    }
    if (timeout_ms > 0u && timeout_ms < recv_slice_ms){
        recv_slice_ms = timeout_ms;
    }
    (void)qos_socket_set_recv_timeout(fd, recv_slice_ms);

    q[qi++] = (unsigned char)(dns_id >> 8);
    q[qi++] = (unsigned char)(dns_id & 0xFFu);
    q[qi++] = 0x01; q[qi++] = 0x00; // RD=1
    q[qi++] = 0x00; q[qi++] = 0x01; // QDCOUNT=1
    q[qi++] = 0x00; q[qi++] = 0x00; // ANCOUNT=0
    q[qi++] = 0x00; q[qi++] = 0x00; // NSCOUNT=0
    q[qi++] = 0x00; q[qi++] = 0x00; // ARCOUNT=0
    qi += (unsigned int)qname_len;
    q[qi++] = 0x00; q[qi++] = 0x01; // QTYPE=A
    q[qi++] = 0x00; q[qi++] = 0x01; // QCLASS=IN

    if (qos_send(fd, q, qi, 0) < 0){
        (void)qos_close(fd);
        dns_id++;
        return QOS_DNS_ERR_SEND;
    }

    {
    unsigned int rounds = (timeout_ms == 0u) ? 1u : ((timeout_ms + recv_slice_ms - 1u) / recv_slice_ms);
    if (rounds == 0u){
        rounds = 1u;
    }
    while (rounds-- > 0u){
        n = qos_recv(fd, r, (unsigned int)sizeof(r), QOS_SOCK_TIMEOUT_USE_SOCKET);
        if (n > 0){
            if (n < 12){
                // Ignore malformed short packet and keep waiting.
            } else if (qos_be16_read(&r[0]) == dns_id){
                unsigned short flags = qos_be16_read(&r[2]);
                unsigned short qdcount = qos_be16_read(&r[4]);
                unsigned short ancount = qos_be16_read(&r[6]);
                unsigned int rcode = (unsigned int)(flags & 0x000Fu);
                unsigned int qr = (unsigned int)((flags >> 15) & 1u);
                int off = 12;
                if (qr == 1u && rcode == 0u){
                    int parse_ok = 1;
                    for (unsigned int i = 0; i < qdcount; i++){
                        off = qos_dns_skip_name(r, n, off);
                        if (off < 0 || off + 4 > n){
                            parse_ok = 0;
                            break;
                        }
                        off += 4;
                    }
                    if (parse_ok){
                        for (unsigned int i = 0; i < ancount; i++){
                            unsigned short type;
                            unsigned short classv;
                            unsigned short rdlen;
                            off = qos_dns_skip_name(r, n, off);
                            if (off < 0 || off + 10 > n){
                                parse_ok = 0;
                                break;
                            }
                            type = qos_be16_read(&r[off + 0]);
                            classv = qos_be16_read(&r[off + 2]);
                            rdlen = qos_be16_read(&r[off + 8]);
                            off += 10;
                            if (off + rdlen > n){
                                parse_ok = 0;
                                break;
                            }
                            if (type == 1u && classv == 1u && rdlen == 4u){
                                out_ip[0] = r[off + 0];
                                out_ip[1] = r[off + 1];
                                out_ip[2] = r[off + 2];
                                out_ip[3] = r[off + 3];
                                (void)qos_close(fd);
                                dns_id++;
                                return 0;
                            }
                            off += rdlen;
                        }
                        // Valid matching DNS response without A answer.
                        (void)qos_close(fd);
                        dns_id++;
                        return QOS_DNS_ERR_NO_A;
                    }
                }
                // Matching ID but no A record/rcode error: treat as definitive.
                if (qr == 1u && rcode != 0u){
                    (void)qos_close(fd);
                    dns_id++;
                    return QOS_DNS_ERR_PARSE;
                }
            }
            // Different ID or irrelevant packet: keep waiting until timeout.
        }
    }
    }

    (void)qos_close(fd);
    dns_id++;
    return QOS_DNS_ERR_TIMEOUT;
}

#endif
