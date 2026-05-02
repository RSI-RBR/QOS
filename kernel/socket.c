#include "socket.h"
#include "process.h"
#include "udp.h"
#include "net.h"
#include "timer.h"
#include "tcp.h"
#include "spinlock.h"

#define SOCKET_MAX_GLOBAL 32
#define SOCKET_MAX_PER_PROCESS 8
#define SOCKET_EPHEMERAL_PORT_BASE 49152u
#define SOCKET_EPHEMERAL_PORT_LAST 65535u
#define SOCKET_STREAM_RX_CAP 16384u

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
    unsigned char stream_rx[SOCKET_STREAM_RX_CAP];
} kernel_socket_t;

static kernel_socket_t g_sockets[SOCKET_MAX_GLOBAL];
static int g_fd_map[MAX_PROCESSES][SOCKET_MAX_PER_PROCESS];
static unsigned short g_next_ephemeral_port = SOCKET_EPHEMERAL_PORT_BASE;
static spinlock_t g_socket_lock;

static unsigned char ascii_lower(unsigned char c){
    if (c >= 'A' && c <= 'Z'){
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static int parse_http_get_request(const unsigned char* data,
                                  unsigned int len,
                                  char* host,
                                  unsigned int host_cap,
                                  char* path,
                                  unsigned int path_cap){
    if (!data || len == 0 || !host || host_cap < 2u || !path || path_cap < 2u){
        return -1;
    }
    host[0] = 0;
    path[0] = '/';
    path[1] = 0;

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
            break;
        }

        i = le;
        while (i < len && (data[i] == '\r' || data[i] == '\n')){
            i++;
        }
    }

    return host[0] ? 0 : -1;
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
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    g_next_ephemeral_port = SOCKET_EPHEMERAL_PORT_BASE;
    for (int i = 0; i < SOCKET_MAX_GLOBAL; i++){
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
    if (!valid_pid(pid)){
        return;
    }
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    for (int fd = 0; fd < SOCKET_MAX_PER_PROCESS; fd++){
        int si = g_fd_map[pid][fd];
        if (si >= 0 && si < SOCKET_MAX_GLOBAL){
            clear_socket(&g_sockets[si]);
        }
        g_fd_map[pid][fd] = -1;
    }
    spin_unlock_irqrestore(&g_socket_lock, irq);
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
        clear_socket(&g_sockets[si]);
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
    unsigned int stream_out_cap = 0;
    unsigned char stream_remote_ip[4] = {0, 0, 0, 0};
    char stream_host[128];
    char stream_path[256];

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
        int rc = parse_http_get_request(data, len, stream_host, sizeof(stream_host), stream_path, sizeof(stream_path));
        if (rc != 0){
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
        }
        stream_out_cap = SOCKET_STREAM_RX_CAP - 1u;
        stream_remote_ip[0] = s->remote_ip[0];
        stream_remote_ip[1] = s->remote_ip[1];
        stream_remote_ip[2] = s->remote_ip[2];
        stream_remote_ip[3] = s->remote_ip[3];
        unsigned char* stream_out = s->stream_rx;
        spin_unlock_irqrestore(&g_socket_lock, irq);

        int n = tcp_http_get(stream_remote_ip, stream_host, stream_path, stream_out, stream_out_cap);

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
            s->stream_rx_len = 0;
            s->stream_rx_off = 0;
            spin_unlock_irqrestore(&g_socket_lock, irq);
            return -1;
        }
        if ((unsigned int)n > stream_out_cap){
            n = (int)stream_out_cap;
        }
        s->stream_rx_len = (unsigned int)n;
        s->stream_rx_off = 0;
        spin_unlock_irqrestore(&g_socket_lock, irq);
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

        if (s->type == QOS_SOCK_STREAM){
            if (s->stream_rx_off >= s->stream_rx_len){
                spin_unlock_irqrestore(&g_socket_lock, irq);
                return 0;
            }
            unsigned int available = s->stream_rx_len - s->stream_rx_off;
            unsigned int n = out_cap < available ? out_cap : available;
            for (unsigned int i = 0; i < n; i++){
                out[i] = s->stream_rx[s->stream_rx_off + i];
            }
            s->stream_rx_off += n;
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
    unsigned long irq = spin_lock_irqsave(&g_socket_lock);
    int si = lookup_socket_index(pid, fd);
    if (si < 0){
        spin_unlock_irqrestore(&g_socket_lock, irq);
        return -1;
    }
    clear_socket(&g_sockets[si]);
    g_fd_map[pid][fd] = -1;
    spin_unlock_irqrestore(&g_socket_lock, irq);
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
