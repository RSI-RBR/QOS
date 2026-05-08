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
#define QOS_DNS_ERR_HTTP    (-8)
#define QOS_DNS_ERR_SECURE  (-9)

static inline unsigned short qos_be16_read(const unsigned char* p){
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static inline unsigned char qos_ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static inline int qos_parse_ipv4_text_len(const char* s, unsigned int len, unsigned char out_ip[4]){
    unsigned int i = 0u;
    if (!s || !out_ip || len == 0u){
        return -1;
    }
    for (unsigned int part = 0u; part < 4u; part++){
        unsigned int v = 0u;
        unsigned int digits = 0u;
        while (i < len && s[i] >= '0' && s[i] <= '9'){
            v = (v * 10u) + (unsigned int)(s[i] - '0');
            if (v > 255u){
                return -1;
            }
            i++;
            digits++;
        }
        if (digits == 0u){
            return -1;
        }
        out_ip[part] = (unsigned char)v;
        if (part < 3u){
            if (i >= len || s[i] != '.'){
                return -1;
            }
            i++;
        }
    }
    return (i == len) ? 0 : -1;
}

static inline int qos_dns_host_is_safe(const char* host){
    unsigned int n = 0u;
    int last_dot = 0;
    if (!host || !*host){
        return 0;
    }
    while (host[n]){
        unsigned char c = (unsigned char)host[n];
        int ok_alpha = (qos_ascii_lower(c) >= 'a' && qos_ascii_lower(c) <= 'z');
        int ok_digit = (c >= '0' && c <= '9');
        int ok_dash = (c == '-');
        int ok_dot = (c == '.');
        if (!(ok_alpha || ok_digit || ok_dash || ok_dot)){
            return 0;
        }
        if (ok_dot){
            if (n == 0u || last_dot){
                return 0;
            }
            last_dot = 1;
        } else{
            last_dot = 0;
        }
        n++;
        if (n > 253u){
            return 0;
        }
    }
    if (n == 0u || last_dot){
        return 0;
    }
    return 1;
}

static inline int qos_http_find_body_offset(const unsigned char* buf, unsigned int len, unsigned int* out_off){
    if (!buf || !out_off || len < 4u){
        return 0;
    }
    for (unsigned int i = 0u; i + 3u < len; i++){
        if (buf[i] == '\r' && buf[i + 1u] == '\n' &&
            buf[i + 2u] == '\r' && buf[i + 3u] == '\n'){
            *out_off = i + 4u;
            return 1;
        }
    }
    return 0;
}

static inline int qos_http_status_code(const unsigned char* buf, unsigned int len){
    unsigned int i = 0u;
    if (!buf || len < 12u){
        return -1;
    }
    if (!(buf[0] == 'H' && buf[1] == 'T' && buf[2] == 'T' && buf[3] == 'P' && buf[4] == '/')){
        return -1;
    }
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n'){
        i++;
    }
    if (i >= len || buf[i] != ' '){
        return -1;
    }
    i++;
    if (i + 2u >= len || buf[i] < '0' || buf[i] > '9' ||
        buf[i + 1u] < '0' || buf[i + 1u] > '9' ||
        buf[i + 2u] < '0' || buf[i + 2u] > '9'){
        return -1;
    }
    return ((int)(buf[i] - '0') * 100) + ((int)(buf[i + 1u] - '0') * 10) + (int)(buf[i + 2u] - '0');
}

static inline int qos_json_status_is_noerror(const unsigned char* body, unsigned int len){
    if (!body || len < 9u){
        return -1;
    }
    for (unsigned int i = 0u; i + 8u < len; i++){
        if (body[i] == '"' &&
            body[i + 1u] == 'S' && body[i + 2u] == 't' && body[i + 3u] == 'a' &&
            body[i + 4u] == 't' && body[i + 5u] == 'u' && body[i + 6u] == 's' &&
            body[i + 7u] == '"'){
            unsigned int j = i + 8u;
            while (j < len && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n')){
                j++;
            }
            if (j >= len || body[j] != ':'){
                return -1;
            }
            j++;
            while (j < len && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n')){
                j++;
            }
            if (j >= len || body[j] < '0' || body[j] > '9'){
                return -1;
            }
            int v = 0;
            while (j < len && body[j] >= '0' && body[j] <= '9'){
                v = (v * 10) + (int)(body[j] - '0');
                j++;
            }
            return (v == 0) ? 1 : 0;
        }
    }
    return -1;
}

static inline int qos_json_extract_first_a_data(const unsigned char* body, unsigned int len, unsigned char out_ip[4]){
    if (!body || !out_ip || len < 8u){
        return -1;
    }
    for (unsigned int i = 0u; i + 7u < len; i++){
        if (body[i] == '"' &&
            qos_ascii_lower(body[i + 1u]) == 'd' &&
            qos_ascii_lower(body[i + 2u]) == 'a' &&
            qos_ascii_lower(body[i + 3u]) == 't' &&
            qos_ascii_lower(body[i + 4u]) == 'a' &&
            body[i + 5u] == '"'){
            unsigned int j = i + 6u;
            while (j < len && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n')){
                j++;
            }
            if (j >= len || body[j] != ':'){
                continue;
            }
            j++;
            while (j < len && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n')){
                j++;
            }
            if (j >= len || body[j] != '"'){
                continue;
            }
            j++;
            unsigned int start = j;
            while (j < len && body[j] != '"' && body[j] != '\r' && body[j] != '\n'){
                j++;
            }
            if (j <= start || j >= len || body[j] != '"'){
                continue;
            }
            if (qos_parse_ipv4_text_len((const char*)&body[start], j - start, out_ip) == 0){
                return 0;
            }
        }
    }
    return -1;
}

static inline int qos_append_str(char* dst, unsigned int cap, unsigned int* io_idx, const char* src){
    if (!dst || !io_idx || !src || cap == 0u){
        return -1;
    }
    while (*src){
        if ((*io_idx + 1u) >= cap){
            return -1;
        }
        dst[*io_idx] = *src;
        (*io_idx)++;
        src++;
    }
    dst[*io_idx] = 0;
    return 0;
}

static inline int qos_dns_resolve_a_doh_once(const char* host,
                                             const unsigned char resolver_ip[4],
                                             const char* resolver_host,
                                             const char* resolver_path_prefix,
                                             unsigned char out_ip[4],
                                             unsigned int timeout_ms){
    qos_sockaddr_in_t sa;
    char req[700];
    unsigned char resp[4096];
    unsigned int rq = 0u;
    unsigned int used = 0u;
    unsigned int body_off = 0u;
    unsigned int slice_ms;
    unsigned long start_tick;
    int fd;
    int http_status;
    int dns_status_ok;

    if (!host || !resolver_ip || !resolver_host || !resolver_path_prefix || !out_ip){
        return QOS_DNS_ERR_PARAM;
    }
    if (!qos_dns_host_is_safe(host)){
        return QOS_DNS_ERR_PARAM;
    }
    if (timeout_ms == 0u){
        timeout_ms = 3500u;
    }
    slice_ms = timeout_ms < 350u ? timeout_ms : 350u;
    if (slice_ms == 0u){
        slice_ms = 150u;
    }

    fd = qos_socket(QOS_AF_INET, QOS_SOCK_STREAM, 0);
    if (fd < 0){
        return QOS_DNS_ERR_SOCKET;
    }

    sa.family = QOS_AF_INET;
    sa.port = 443u;
    sa.addr[0] = resolver_ip[0];
    sa.addr[1] = resolver_ip[1];
    sa.addr[2] = resolver_ip[2];
    sa.addr[3] = resolver_ip[3];
    for (int i = 0; i < (int)sizeof(sa.reserved); i++){
        sa.reserved[i] = 0;
    }
    if (qos_connect(fd, &sa, (unsigned int)sizeof(sa)) != 0){
        (void)qos_close(fd);
        return QOS_DNS_ERR_CONNECT;
    }
    (void)qos_socket_set_recv_timeout(fd, slice_ms);

    req[0] = 0;
    if (qos_append_str(req, (unsigned int)sizeof(req), &rq, "GET ") != 0 ||
        qos_append_str(req, (unsigned int)sizeof(req), &rq, resolver_path_prefix) != 0 ||
        qos_append_str(req, (unsigned int)sizeof(req), &rq, host) != 0 ||
        qos_append_str(req, (unsigned int)sizeof(req), &rq,
                       "&type=A&cd=0&do=1 HTTP/1.1\r\nHost: ") != 0 ||
        qos_append_str(req, (unsigned int)sizeof(req), &rq, resolver_host) != 0 ||
        qos_append_str(req, (unsigned int)sizeof(req), &rq,
                       "\r\nAccept: application/dns-json\r\n"
                       "User-Agent: QOS-DNS/0.1\r\n"
                       "X-QOS-PQ-SIG: mldsa65-first\r\n"
                       "Connection: close\r\n\r\n") != 0){
        (void)qos_close(fd);
        return QOS_DNS_ERR_PARAM;
    }

    if (qos_send(fd, req, rq, 0) < 0){
        (void)qos_close(fd);
        return QOS_DNS_ERR_SEND;
    }

    start_tick = qos_get_ticks();
    while (used + 1u < (unsigned int)sizeof(resp)){
        int n;
        unsigned long elapsed = qos_get_ticks() - start_tick;
        if (elapsed >= timeout_ms){
            break;
        }

        n = qos_recv(fd, &resp[used], (unsigned int)sizeof(resp) - 1u - used, QOS_SOCK_TIMEOUT_USE_SOCKET);
        if (n == QOS_SOCK_ERR_AGAIN){
            continue;
        }
        if (n < 0){
            (void)qos_close(fd);
            return QOS_DNS_ERR_TIMEOUT;
        }
        if (n == 0){
            break;
        }
        used += (unsigned int)n;
        resp[used] = 0;

        if (qos_http_find_body_offset(resp, used, &body_off)){
            if (qos_json_extract_first_a_data(&resp[body_off], used - body_off, out_ip) == 0){
                int st = qos_json_status_is_noerror(&resp[body_off], used - body_off);
                (void)qos_close(fd);
                if (st == 1 || st == -1){
                    return 0;
                }
                return QOS_DNS_ERR_PARSE;
            }
        }
    }

    (void)qos_close(fd);
    if (used == 0u){
        return QOS_DNS_ERR_TIMEOUT;
    }

    http_status = qos_http_status_code(resp, used);
    if (http_status < 200 || http_status >= 300){
        return QOS_DNS_ERR_HTTP;
    }
    if (!qos_http_find_body_offset(resp, used, &body_off)){
        return QOS_DNS_ERR_PARSE;
    }

    dns_status_ok = qos_json_status_is_noerror(&resp[body_off], used - body_off);
    if (dns_status_ok == 0){
        return QOS_DNS_ERR_NO_A;
    }
    if (qos_json_extract_first_a_data(&resp[body_off], used - body_off, out_ip) == 0){
        return 0;
    }
    return QOS_DNS_ERR_NO_A;
}

static inline int qos_dns_resolve_a_secure_socket(const char* host,
                                                  unsigned char out_ip[4],
                                                  unsigned int timeout_ms){
    static const unsigned char doh_google_0[4] = {8, 8, 8, 8};
    static const unsigned char doh_google_1[4] = {8, 8, 4, 4};
    static const unsigned char doh_cf_0[4] = {1, 1, 1, 1};
    int rc;

    rc = qos_dns_resolve_a_doh_once(host, doh_google_0, "dns.google", "/resolve?name=", out_ip, timeout_ms);
    if (rc == 0){
        return 0;
    }
    rc = qos_dns_resolve_a_doh_once(host, doh_google_1, "dns.google", "/resolve?name=", out_ip, timeout_ms);
    if (rc == 0){
        return 0;
    }
    rc = qos_dns_resolve_a_doh_once(host, doh_cf_0, "cloudflare-dns.com", "/dns-query?name=", out_ip, timeout_ms);
    if (rc == 0){
        return 0;
    }
    return QOS_DNS_ERR_SECURE;
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
