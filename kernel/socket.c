#include "socket.h"
#include "process.h"
#include "udp.h"
#include "net.h"
#include "timer.h"
#include "tcp.h"
#include "spinlock.h"
#include "uart.h"

#define SOCKET_MAX_GLOBAL 32
#define SOCKET_MAX_PER_PROCESS 8
#define SOCKET_EPHEMERAL_PORT_BASE 49152u
#define SOCKET_EPHEMERAL_PORT_LAST 65535u
#define SOCKET_STREAM_RX_CAP 524288u
#define SOCKET_STREAM_POOL_SLOTS 2

typedef struct {
    int used;
    int owner_pid;
    int domain;
    int type;
    int protocol;
    int nonblocking;
    unsigned int recv_timeout_ms;
    unsigned short local_port;
    unsigned short remote_port;
    unsigned char remote_ip[4];
    int connected;
    unsigned int stream_rx_len;
    unsigned int stream_rx_off;
    int stream_pool_slot;
    int stream_req_pending;
    unsigned short stream_req_port;
    unsigned char stream_req_ip[4];
    int stream_req_accept_gzip;
    int stream_req_pq_sig_pref;
    char stream_req_host[128];
    char stream_req_path[256];
} kernel_socket_t;

static kernel_socket_t g_sockets[SOCKET_MAX_GLOBAL];
static int g_fd_map[MAX_PROCESSES][SOCKET_MAX_PER_PROCESS];
static unsigned short g_next_ephemeral_port = SOCKET_EPHEMERAL_PORT_BASE;
static spinlock_t g_socket_lock;
static spinlock_t g_socket_stream_lock;
static unsigned char g_stream_http_tmp[SOCKET_STREAM_RX_CAP];
static unsigned char g_stream_rx_pool[SOCKET_STREAM_POOL_SLOTS][SOCKET_STREAM_RX_CAP];
static int g_stream_pool_owner[SOCKET_STREAM_POOL_SLOTS];

static unsigned char ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int header_value_has_token(const unsigned char* p, unsigned int len, const char* token){
    unsigned int tlen = 0u;
    if (!p || !token){
        return 0;
    }
    while (token[tlen]){
        tlen++;
    }
    if (tlen == 0u || len < tlen){
        return 0;
    }
    for (unsigned int i = 0; i + tlen <= len; i++){
        unsigned int j = 0u;
        while (j < tlen && ascii_lower(p[i + j]) == ascii_lower((unsigned char)token[j])){
            j++;
        }
        if (j == tlen){
            return 1;
        }
    }
    return 0;
}

static int parse_http_get_request(const unsigned char* data,
                                  unsigned int len,
                                  char* host,
                                  unsigned int host_cap,
                                  char* path,
                                  unsigned int path_cap,
                                  int* accept_gzip,
                                  int* pq_sig_pref){
    if (!data || len == 0 || !host || host_cap < 2u || !path || path_cap < 2u ||
        !accept_gzip || !pq_sig_pref){
        return -1;
    }
    host[0] = 0;
    path[0] = '/';
    path[1] = 0;
    *accept_gzip = 0;
    *pq_sig_pref = 0;

    unsigned int line0_end = 0;
    while (line0_end < len && data[line0_end] != '\r' && data[line0_end] != '\n'){
        line0_end++;
    }
    if (line0_end >= 5u &&
        data[0] == 'G' && data[1] == 'E' && data[2] == 'T' && data[3] == ' '){
        unsigned int s = 4u;
        unsigned int e = s;
        while (e < line0_end && data[e] != ' '){
            e++;
        }
        unsigned int n = e > s ? (e - s) : 0u;
        if (n >= path_cap){
            n = path_cap - 1u;
        }
        if (n > 0){
            for (unsigned int i = 0; i < n; i++){
                path[i] = (char)data[s + i];
            }
            path[n] = 0;
        }
    }

    unsigned int i = line0_end;
    while (i < len && (data[i] == '\r' || data[i] == '\n')){
        i++;
    }

    while (i < len){
        unsigned int ls = i;
        unsigned int le = ls;
        while (le < len && data[le] != '\r' && data[le] != '\n'){
            le++;
        }
        if (le == ls){
            break;
        }

        if ((le - ls) >= 5u &&
            ascii_lower(data[ls + 0]) == 'h' &&
            ascii_lower(data[ls + 1]) == 'o' &&
            ascii_lower(data[ls + 2]) == 's' &&
            ascii_lower(data[ls + 3]) == 't' &&
            data[ls + 4] == ':'){
            unsigned int vs = ls + 5u;
            while (vs < le && (data[vs] == ' ' || data[vs] == '\t')){
                vs++;
            }
            unsigned int ve = le;
            while (ve > vs && (data[ve - 1] == ' ' || data[ve - 1] == '\t')){
                ve--;
            }
            unsigned int n = ve > vs ? (ve - vs) : 0u;
            if (n >= host_cap){
                n = host_cap - 1u;
            }
            for (unsigned int k = 0; k < n; k++){
                host[k] = (char)data[vs + k];
            }
            host[n] = 0;
        } else if ((le - ls) >= 16u &&
                   ascii_lower(data[ls + 0]) == 'a' &&
                   ascii_lower(data[ls + 1]) == 'c' &&
                   ascii_lower(data[ls + 2]) == 'c' &&
                   ascii_lower(data[ls + 3]) == 'e' &&
                   ascii_lower(data[ls + 4]) == 'p' &&
                   ascii_lower(data[ls + 5]) == 't' &&
                   data[ls + 6] == '-' &&
                   ascii_lower(data[ls + 7]) == 'e' &&
                   ascii_lower(data[ls + 8]) == 'n' &&
                   ascii_lower(data[ls + 9]) == 'c' &&
                   ascii_lower(data[ls + 10]) == 'o' &&
                   ascii_lower(data[ls + 11]) == 'd' &&
                   ascii_lower(data[ls + 12]) == 'i' &&
                   ascii_lower(data[ls + 13]) == 'n' &&
                   ascii_lower(data[ls + 14]) == 'g' &&
                   data[ls + 15] == ':'){
            unsigned int vs = ls + 16u;
            while (vs < le && (data[vs] == ' ' || data[vs] == '\t')){
                vs++;
            }
            if (header_value_has_token(&data[vs], le - vs, "gzip")){
                *accept_gzip = 1;
            }
        } else if ((le - ls) >= 14u &&
                   ascii_lower(data[ls + 0]) == 'x' &&
                   data[ls + 1] == '-' &&
                   ascii_lower(data[ls + 2]) == 'q' &&
                   ascii_lower(data[ls + 3]) == 'o' &&
                   ascii_lower(data[ls + 4]) == 's' &&
                   data[ls + 5] == '-' &&
                   ascii_lower(data[ls + 6]) == 'p' &&
                   ascii_lower(data[ls + 7]) == 'q' &&
                   data[ls + 8] == '-' &&
                   ascii_lower(data[ls + 9]) == 's' &&
                   ascii_lower(data[ls + 10]) == 'i' &&
                   ascii_lower(data[ls + 11]) == 'g' &&
                   data[ls + 12] == ':'){
            unsigned int vs = ls + 13u;
            while (vs < le && (data[vs] == ' ' || data[vs] == '\t')){
                vs++;
            }
            if (header_value_has_token(&data[vs], le - vs, "mldsa65-first") ||
                header_value_has_token(&data[vs], le - vs, "prefer")){
                *pq_sig_pref = 1;
            }
        }

        i = le;
        while (i < len && (data[i] == '\r' || data[i] == '\n')){
            i++;
        }
    }

    return host[0] ? 0 : -1;
}

static void release_stream_slot_locked(int si){
    if (si < 0 || si >= SOCKET_MAX_GLOBAL){
        return;
    }
    int slot = g_sockets[si].stream_pool_slot;
    if (slot >= 0 && slot < SOCKET_STREAM_POOL_SLOTS &&
        g_stream_pool_owner[slot] == si){
        g_stream_pool_owner[slot] = -1;
    }
    g_sockets[si].stream_pool_slot = -1;
    g_sockets[si].stream_rx_len = 0;
    g_sockets[si].stream_rx_off = 0;
}

static int ensure_stream_slot_locked(int si){
    if (si < 0 || si >= SOCKET_MAX_GLOBAL){
        return -1;
    }
    int slot = g_sockets[si].stream_pool_slot;
    if (slot >= 0 && slot < SOCKET_STREAM_POOL_SLOTS &&
        g_stream_pool_owner[slot] == si){
        return slot;
    }
    for (int i = 0; i < SOCKET_STREAM_POOL_SLOTS; i++){
        if (g_stream_pool_owner[i] < 0){
            g_stream_pool_owner[i] = si;
            g_sockets[si].stream_pool_slot = i;
            return i;
        }
    }
    return -1;
}

static void clear_socket(kernel_socket_t* s){
    if (!s){
        return;
    }
    s->used = 0;
    s->owner_pid = -1;
    s->domain = 0;
    s->type = 0;
    s->protocol = 0;
    s->nonblocking = 0;
    s->recv_timeout_ms = 0;
    s->local_port = 0;
    s->remote_port = 0;
    s->remote_ip[0] = 0;
    s->remote_ip[1] = 0;
    s->remote_ip[2] = 0;
    s->remote_ip[3] = 0;
    s->connected = 0;
    s->stream_rx_len = 0;
    s->stream_rx_off = 0;
    s->stream_pool_slot = -1;
    s->stream_req_pending = 0;
    s->stream_req_port = 0;
    s->stream_req_ip[0] = 0;
    s->stream_req_ip[1] = 0;
    s->stream_req_ip[2] = 0;
    s->stream_req_ip[3] = 0;
    s->stream_req_accept_gzip = 0;
    s->stream_req_pq_sig_pref = 0;
    s->stream_req_host[0] = 0;
    s->stream_req_path[0] = 0;
}

static void clear_socket_at_locked(int si){
    if (si < 0 || si >= SOCKET_MAX_GLOBAL){
        return;
    }
    release_stream_slot_locked(si);
    clear_socket(&g_sockets[si]);
}

static int valid_pid(int pid){
    return pid >= 0 && pid < MAX_PROCESSES;
}

static int valid_fd(int fd){
    return fd >= 0 && fd < SOCKET_MAX_PER_PROCESS;
}

static int allocate_global_socket(void){
    for (int i = 0; i < SOCKET_MAX_GLOBAL; i++){
        if (!g_sockets[i].used){
            g_sockets[i].used = 1;
            return i;
        }
    }
    return -1;
}

static int allocate_process_fd(int pid){
    for (int i = 0; i < SOCKET_MAX_PER_PROCESS; i++){
        if (g_fd_map[pid][i] < 0){
            return i;
        }
    }
    return -1;
}

static unsigned short allocate_ephemeral_port(void){
    unsigned short p = g_next_ephemeral_port;
    g_next_ephemeral_port++;
    if (g_next_ephemeral_port < SOCKET_EPHEMERAL_PORT_BASE || g_next_ephemeral_port > SOCKET_EPHEMERAL_PORT_LAST){
        g_next_ephemeral_port = SOCKET_EPHEMERAL_PORT_BASE;
    }
    if (p == 0){
        p = SOCKET_EPHEMERAL_PORT_BASE;
    }
    return p;
}

static int lookup_socket_index(int pid, int fd){
    if (!valid_pid(pid) || !valid_fd(fd)){
        return -1;
    }
    int si = g_fd_map[pid][fd];
    if (si < 0 || si >= SOCKET_MAX_GLOBAL){
        return -1;
    }
    if (!g_sockets[si].used || g_sockets[si].owner_pid != pid){
        return -1;
    }
    return si;
}

void socket_layer_init(void){
    spinlock_init(&g_socket_lock);
    spinlock_init(&g_socket_stream_lock);
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    g_next_ephemeral_port = SOCKET_EPHEMERAL_PORT_BASE;
    for (int i = 0; i < SOCKET_STREAM_POOL_SLOTS; i++){
        g_stream_pool_owner[i] = -1;
    }
    for (int i = 0; i < SOCKET_MAX_GLOBAL; i++){
        g_sockets[i].stream_pool_slot = -1;
        clear_socket(&g_sockets[i]);
    }
    for (int p = 0; p < MAX_PROCESSES; p++){
        for (int fd = 0; fd < SOCKET_MAX_PER_PROCESS; fd++){
            g_fd_map[p][fd] = -1;
        }
    }
    spin_unlock_irqrestore(&g_socket_lock, irq);
}

void socket_close_all_for_pid(int pid){
    int closed_stream = 0;
    if (!valid_pid(pid)){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    for (int fd = 0; fd < SOCKET_MAX_PER_PROCESS; fd++){
        int si = g_fd_map[pid][fd];
        if (si >= 0 && si < SOCKET_MAX_GLOBAL){
            if (g_sockets[si].type == QOS_SOCK_STREAM){
                closed_stream = 1;
            }
            clear_socket_at_locked(si);
        }
        g_fd_map[pid][fd] = -1;
    }
    spin_unlock_irqrestore(&g_socket_lock, irq);
    if (closed_stream){
        tcp_https_stream_close();
    }
}

int ksocket_create(int pid, int domain, int type, int protocol){
    if (!valid_pid(pid)){
        return -1;
    }
    if (domain != QOS_AF_INET){
        return -1;
    }
    if (type != QOS_SOCK_DGRAM && type != QOS_SOCK_STREAM){
        return -1;
    }
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);

    int si = allocate_global_socket();
    if (si < 0){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }
    int fd = allocate_process_fd(pid);
    if (fd < 0){
        clear_socket_at_locked(si);
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }

    kernel_socket_t* s = &g_sockets[si];
    s->owner_pid = pid;
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->nonblocking = 0;
    s->recv_timeout_ms = 3000u;
    s->local_port = 0;
    s->remote_port = 0;
    s->remote_ip[0] = 0;
    s->remote_ip[1] = 0;
    s->remote_ip[2] = 0;
    s->remote_ip[3] = 0;
    s->connected = 0;
    s->stream_rx_len = 0;
    s->stream_rx_off = 0;
    s->stream_pool_slot = -1;
    s->stream_req_pending = 0;
    s->stream_req_port = 0;
    s->stream_req_ip[0] = 0;
    s->stream_req_ip[1] = 0;
    s->stream_req_ip[2] = 0;
    s->stream_req_ip[3] = 0;
    s->stream_req_host[0] = 0;
    s->stream_req_path[0] = 0;

    g_fd_map[pid][fd] = si;
    spin_unlock_irqrestore(&g_socket_lock, irq);
    return fd;
}

int ksocket_connect(int pid, int fd, const qos_sockaddr_in_t* addr, unsigned int addr_len){
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    int si = lookup_socket_index(pid, fd);
    if (si < 0 || !addr || addr_len < sizeof(qos_sockaddr_in_t)){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }

    kernel_socket_t* s = &g_sockets[si];
    if (addr->family != QOS_AF_INET || addr->port == 0){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }
    s->remote_port = addr->port;
    s->remote_ip[0] = addr->addr[0];
    s->remote_ip[1] = addr->addr[1];
    s->remote_ip[2] = addr->addr[2];
    s->remote_ip[3] = addr->addr[3];
    s->connected = 1;
    spin_unlock_irqrestore(&g_socket_lock, irq);
    return 0;
}

int ksocket_send(int pid, int fd, const unsigned char* data, unsigned int len, unsigned int flags){
    (void)flags;
    unsigned short local_port = 0;
    unsigned short remote_port = 0;
    unsigned char remote_ip[4] = {0, 0, 0, 0};
    int type = 0;
    char stream_host[128];
    char stream_path[256];
    int stream_accept_gzip = 0;
    int stream_pq_sig_pref = 0;

    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    int si = lookup_socket_index(pid, fd);
    if (si < 0 || !data || len == 0){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }

    kernel_socket_t* s = &g_sockets[si];
    if (!s->connected){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }

    type = s->type;
    if (type == QOS_SOCK_DGRAM){
        if (s->local_port == 0){
            s->local_port = allocate_ephemeral_port();
        }
        local_port = s->local_port;
        remote_port = s->remote_port;
        remote_ip[0] = s->remote_ip[0];
        remote_ip[1] = s->remote_ip[1];
        remote_ip[2] = s->remote_ip[2];
        remote_ip[3] = s->remote_ip[3];
        spin_unlock_irqrestore(&g_socket_lock, irq);
        if (udp_send(remote_ip, local_port, remote_port, data, len) != 0){
            return -1;
        }
        return (int)len;
    }

    if (type == QOS_SOCK_STREAM){
        int rc = parse_http_get_request(data, len, stream_host, sizeof(stream_host),
                                        stream_path, sizeof(stream_path),
                                        &stream_accept_gzip, &stream_pq_sig_pref);
        if (rc != 0){
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
        }
        // Queue request metadata; actual network fetch happens in recv().
        release_stream_slot_locked(si);
        s->stream_rx_len = 0;
        s->stream_rx_off = 0;
        s->stream_req_port = s->remote_port;
        s->stream_req_ip[0] = s->remote_ip[0];
        s->stream_req_ip[1] = s->remote_ip[1];
        s->stream_req_ip[2] = s->remote_ip[2];
        s->stream_req_ip[3] = s->remote_ip[3];
        s->stream_req_accept_gzip = stream_accept_gzip;
        s->stream_req_pq_sig_pref = stream_pq_sig_pref;
        {
            unsigned int i = 0;
            while (i + 1u < (unsigned int)sizeof(s->stream_req_host) && stream_host[i]){
                s->stream_req_host[i] = stream_host[i];
                i++;
            }
            s->stream_req_host[i] = 0;
        }
        {
            unsigned int i = 0;
            while (i + 1u < (unsigned int)sizeof(s->stream_req_path) && stream_path[i]){
                s->stream_req_path[i] = stream_path[i];
                i++;
            }
            s->stream_req_path[i] = 0;
        }
        s->stream_req_pending = 1;
        spin_unlock_irqrestore(&g_socket_lock, irq);
        tcp_https_stream_close();
        return (int)len;
    }

    spin_unlock_irqrestore(&g_socket_lock, irq);
    return -1;
}

int ksocket_recv(int pid, int fd, unsigned char* out, unsigned int out_cap, unsigned int timeout_ms){
    if (!out || out_cap == 0){
        return -1;
    }
    unsigned long start = system_ticks;
    while (1){
        unsigned short local_port = 0;
        unsigned short remote_port = 0;
        unsigned char remote_ip[4] = {0, 0, 0, 0};
        int nonblocking = 0;
        unsigned int effective_timeout = timeout_ms;
        int wait_forever = 0;
        int stream_fetch_needed = 0;
        unsigned int stream_out_cap = SOCKET_STREAM_RX_CAP - 1u;
        unsigned short stream_remote_port = 0;
        unsigned char stream_remote_ip[4] = {0, 0, 0, 0};
        int stream_accept_gzip = 0;
        int stream_pq_sig_pref = 0;
        char stream_host[128];
        char stream_path[256];

        unsigned long irq = spin_lock_irqsave(&g_socket_lock);
        int si = lookup_socket_index(pid, fd);
        if (si < 0){
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
        }

        kernel_socket_t* s = &g_sockets[si];
        if (!s->connected){
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
        }
        nonblocking = s->nonblocking;

        if (s->type == QOS_SOCK_STREAM){
            if (s->stream_rx_off >= s->stream_rx_len){
                if (effective_timeout == QOS_SOCK_TIMEOUT_USE_SOCKET){
                    effective_timeout = s->recv_timeout_ms;
                }
                if (effective_timeout == QOS_SOCK_TIMEOUT_INFINITE){
                    effective_timeout = 0u;
                }
                stream_remote_port = s->remote_port;
                if (s->stream_req_pending){
                    stream_fetch_needed = 1;
                    stream_out_cap = SOCKET_STREAM_RX_CAP - 1u;
                    stream_remote_port = s->stream_req_port;
                    stream_remote_ip[0] = s->stream_req_ip[0];
                    stream_remote_ip[1] = s->stream_req_ip[1];
                    stream_remote_ip[2] = s->stream_req_ip[2];
                    stream_remote_ip[3] = s->stream_req_ip[3];
                    stream_accept_gzip = s->stream_req_accept_gzip;
                    stream_pq_sig_pref = s->stream_req_pq_sig_pref;
                    {
                        unsigned int i = 0;
                        while (i + 1u < (unsigned int)sizeof(stream_host) && s->stream_req_host[i]){
                            stream_host[i] = s->stream_req_host[i];
                            i++;
                        }
                        stream_host[i] = 0;
                    }
                    {
                        unsigned int i = 0;
                        while (i + 1u < (unsigned int)sizeof(stream_path) && s->stream_req_path[i]){
                            stream_path[i] = s->stream_req_path[i];
                            i++;
                        }
                        stream_path[i] = 0;
                    }
                    s->stream_req_pending = 0;
                }
                spin_unlock_irqrestore(&g_socket_lock, irq);
                if (stream_fetch_needed || stream_remote_port == 443u){
                    unsigned long sio_irq = spin_lock_irqsave(&g_socket_stream_lock);
                    int n = 0;
                    if (stream_fetch_needed && stream_remote_port == 443u){
                        uart_puts("HTTPS profile: kex_pref=");
                        uart_puts(tcp_tls13_pq_kex_offered() ? "X25519+ML-KEM-768->X25519" : "X25519");
                        uart_puts(", kex_pq_advertised=");
                        uart_puts(tcp_tls13_pq_kex_offered() ? "yes" : "no");
                        uart_puts(", sig_advertised=");
                        uart_puts(tcp_tls13_pq_sig_offered() ? "ML-DSA-first+fallback" : "classic-only");
                        uart_puts(", sig_pref_req=");
                        uart_puts(stream_pq_sig_pref ? "ML-DSA65-first" : "default");
                        uart_puts(", x509_hostname=on, x509_chain_sig=RSA+ECDSA, certverify=RSA+ECDSA\n");
                        n = tcp_https_stream_start_ex(stream_remote_ip, stream_host, stream_path,
                                                      g_stream_http_tmp, stream_out_cap,
                                                      stream_accept_gzip,
                                                      stream_pq_sig_pref);
                        if (n >= 0){
                            unsigned short cert_verify_alg = tcp_tls13_last_cert_verify_alg();
                            uart_puts("HTTPS negotiated kex=");
                            uart_puts(tcp_tls13_pq_kex_active() ? "X25519+ML-KEM-768" : "X25519");
                            uart_puts(" sig=");
                            uart_puts(tcp_tls13_sigalg_name(cert_verify_alg));
                            uart_puts(" (");
                            uart_puthex((unsigned int)cert_verify_alg);
                            uart_puts(")");
                            uart_puts(" first_chunk=");
                            uart_putdec((unsigned long)n);
                            uart_puts("\n");
                        }
                    } else if (stream_fetch_needed){
                        tcp_https_stream_close();
                        n = tcp_http_get_ex(stream_remote_ip, stream_host, stream_path,
                                            g_stream_http_tmp, stream_out_cap,
                                            stream_accept_gzip);
                    } else{
                        n = tcp_https_stream_read(g_stream_http_tmp, SOCKET_STREAM_RX_CAP - 1u, effective_timeout);
                    }
                    spin_unlock_irqrestore(&g_socket_stream_lock, sio_irq);

                    irq = spin_lock_irqsave(&g_socket_lock);
                    si = lookup_socket_index(pid, fd);
                    if (si < 0){
                        spin_unlock_irqrestore(&g_socket_lock, irq);
                        return -1;
                    }
                    s = &g_sockets[si];
                    if (s->type != QOS_SOCK_STREAM){
                        spin_unlock_irqrestore(&g_socket_lock, irq);
                        return -1;
                    }
                    if (n < 0){
                        release_stream_slot_locked(si);
                        s->stream_rx_len = 0;
                        s->stream_rx_off = 0;
                        spin_unlock_irqrestore(&g_socket_lock, irq);
                        return n;
                    }
                    if (n == 0){
                        release_stream_slot_locked(si);
                        s->stream_rx_len = 0;
                        s->stream_rx_off = 0;
                        spin_unlock_irqrestore(&g_socket_lock, irq);
                        if (stream_remote_port == 443u && tcp_https_stream_active()){
                            if (nonblocking){
                                return QOS_SOCK_ERR_AGAIN;
                            }
                            if (effective_timeout != 0u &&
                                (unsigned long)(system_ticks - start) >= (unsigned long)effective_timeout){
                                return 0;
                            }
                            continue;
                        }
                        return 0;
                    }
                    if ((unsigned int)n > stream_out_cap){
                        n = (int)stream_out_cap;
                    }
                    int stream_slot = ensure_stream_slot_locked(si);
                    if (stream_slot < 0){
                        s->stream_rx_len = 0;
                        s->stream_rx_off = 0;
                        spin_unlock_irqrestore(&g_socket_lock, irq);
                        return -1;
                    }
                    for (int i = 0; i < n; i++){
                        g_stream_rx_pool[stream_slot][i] = g_stream_http_tmp[i];
                    }
                    s->stream_rx_len = (unsigned int)n;
                    s->stream_rx_off = 0;
                    spin_unlock_irqrestore(&g_socket_lock, irq);
                    continue;
                }
                return 0;
            }
            int stream_slot = s->stream_pool_slot;
            if (stream_slot < 0 || stream_slot >= SOCKET_STREAM_POOL_SLOTS ||
                g_stream_pool_owner[stream_slot] != si){
                spin_unlock_irqrestore(&g_socket_lock, irq);
                return -1;
            }
            unsigned int available = s->stream_rx_len - s->stream_rx_off;
            unsigned int n = out_cap < available ? out_cap : available;
            for (unsigned int i = 0; i < n; i++){
                out[i] = g_stream_rx_pool[stream_slot][s->stream_rx_off + i];
            }
            s->stream_rx_off += n;
            if (s->stream_rx_off >= s->stream_rx_len){
                release_stream_slot_locked(si);
            }
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return (int)n;
        }

        if (s->type != QOS_SOCK_DGRAM){
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
        }

        local_port = s->local_port;
        remote_port = s->remote_port;
        remote_ip[0] = s->remote_ip[0];
        remote_ip[1] = s->remote_ip[1];
        remote_ip[2] = s->remote_ip[2];
        remote_ip[3] = s->remote_ip[3];
        nonblocking = s->nonblocking;

        if (effective_timeout == QOS_SOCK_TIMEOUT_USE_SOCKET){
            effective_timeout = s->recv_timeout_ms;
        }
        if (effective_timeout == QOS_SOCK_TIMEOUT_INFINITE){
            wait_forever = 1;
        }
        spin_unlock_irqrestore(&g_socket_lock, irq);

        udp_meta_t meta;
        int n;
        (void)net_poll();
        n = udp_recv_filtered(local_port, 1, remote_ip, remote_port, out, out_cap, &meta);
        if (n != 0){
            return n;
        }
        if (nonblocking){
            return QOS_SOCK_ERR_AGAIN;
        }
        if (!wait_forever &&
            (unsigned long)(system_ticks - start) >= (unsigned long)effective_timeout){
            return 0;
        }
    }
}

int ksocket_close(int pid, int fd){
    int close_stream = 0;
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    int si = lookup_socket_index(pid, fd);
    if (si < 0){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }
    if (g_sockets[si].type == QOS_SOCK_STREAM){
        close_stream = 1;
    }
    clear_socket_at_locked(si);
    g_fd_map[pid][fd] = -1;
    spin_unlock_irqrestore(&g_socket_lock, irq);
    if (close_stream){
        tcp_https_stream_close();
    }
    return 0;
}

int ksocket_setopt(int pid, int fd, int opt, unsigned int value){
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    int si = lookup_socket_index(pid, fd);
    if (si < 0){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }

    kernel_socket_t* s = &g_sockets[si];
    switch (opt){
        case QOS_SOCKOPT_NONBLOCK:
            s->nonblocking = value ? 1 : 0;
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return 0;
        case QOS_SOCKOPT_RCVTIMEO_MS:
            s->recv_timeout_ms = value;
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return 0;
        default:
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
    }
}
