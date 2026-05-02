#ifndef SOCKET_H
#define SOCKET_H

#define QOS_AF_INET 2

#define QOS_SOCK_STREAM 1
#define QOS_SOCK_DGRAM 2

#define QOS_SOCK_ERR_AGAIN (-11)

#define QOS_SOCK_TIMEOUT_USE_SOCKET 0xFFFFFFFFu
#define QOS_SOCK_TIMEOUT_INFINITE   0xFFFFFFFEu

#define QOS_SOCKOPT_NONBLOCK      1
#define QOS_SOCKOPT_RCVTIMEO_MS   2

typedef struct {
    unsigned short family;
    unsigned short port;
    unsigned char addr[4];
    unsigned char reserved[8];
} qos_sockaddr_in_t;

void socket_layer_init(void);
void socket_close_all_for_pid(int pid);

int ksocket_create(int pid, int domain, int type, int protocol);
int ksocket_connect(int pid, int fd, const qos_sockaddr_in_t* addr, unsigned int addr_len);
int ksocket_send(int pid, int fd, const unsigned char* data, unsigned int len, unsigned int flags);
int ksocket_recv(int pid, int fd, unsigned char* out, unsigned int out_cap, unsigned int timeout_ms);
int ksocket_setopt(int pid, int fd, int opt, unsigned int value);
int ksocket_close(int pid, int fd);

#endif
