#ifndef SYSCALL_H
#define SYSCALL_H

#include "udp.h"
#include "socket.h"

enum {
    SYS_PUTC = 0,
    SYS_PUTS = 1,
    SYS_SLEEP = 2,
    SYS_EXIT = 3,
    SYS_FB_CLEAR = 4,
    SYS_FB_GET_WIDTH = 5,
    SYS_FB_GET_HEIGHT = 6,
    SYS_FB_RECT = 7,
    SYS_FB_PRESENT = 8,
    SYS_TRY_GETC = 9,
    SYS_RUN_PROGRAM = 10,
    SYS_NET_DUMP_STATS = 11,
    SYS_NET_SEND_TEST_FRAME = 12,
    SYS_NET_POLL = 13,
    SYS_NET_RECV_RAW = 14,
    SYS_NET_SEND_RAW = 15,
    SYS_USB_DUMP_INFO = 16,
    SYS_NET_PING_GATEWAY = 17,
    SYS_NET_UDP_SEND_PROBE = 18,
    SYS_NET_UDP_RECV = 19,
    SYS_NET_UDP_SEND = 20,
    SYS_NET_TCP_HTTP_GET = 21,
    SYS_SOCKET_CREATE = 22,
    SYS_SOCKET_CONNECT = 23,
    SYS_SOCKET_SEND = 24,
    SYS_SOCKET_RECV = 25,
    SYS_SOCKET_CLOSE = 26,
    SYS_SOCKET_SETOPT = 27
};

void* syscall_handle(void* frame_sp, unsigned long esr);

#ifdef QOS_USERSPACE
static inline unsigned long qos_syscall0(unsigned long n){
    register unsigned long x0 asm("x0") = 0;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "cc", "memory");
    return x0;
}

static inline unsigned long qos_syscall1(unsigned long n, unsigned long a0){
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "cc", "memory");
    return x0;
}

static inline unsigned long qos_syscall2(unsigned long n, unsigned long a0, unsigned long a1){
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x1 asm("x1") = a1;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0), "+r"(x1)
        : "r"(x8)
        : "x2", "x3", "x4", "x5", "x6", "x7", "cc", "memory");
    return x0;
}

static inline unsigned long qos_syscall3(unsigned long n, unsigned long a0, unsigned long a1, unsigned long a2){
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x1 asm("x1") = a1;
    register unsigned long x2 asm("x2") = a2;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2)
        : "r"(x8)
        : "x3", "x4", "x5", "x6", "x7", "cc", "memory");
    return x0;
}

static inline unsigned long qos_syscall4(unsigned long n, unsigned long a0, unsigned long a1,
                                         unsigned long a2, unsigned long a3){
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x1 asm("x1") = a1;
    register unsigned long x2 asm("x2") = a2;
    register unsigned long x3 asm("x3") = a3;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        : "r"(x8)
        : "x4", "x5", "x6", "x7", "cc", "memory");
    return x0;
}

static inline unsigned long qos_syscall5(unsigned long n, unsigned long a0, unsigned long a1,
                                         unsigned long a2, unsigned long a3, unsigned long a4){
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x1 asm("x1") = a1;
    register unsigned long x2 asm("x2") = a2;
    register unsigned long x3 asm("x3") = a3;
    register unsigned long x4 asm("x4") = a4;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
        : "r"(x8)
        : "x5", "x6", "x7", "cc", "memory");
    return x0;
}

static inline void qos_putc(char c){
    (void)qos_syscall1(SYS_PUTC, (unsigned long)(unsigned char)c);
}

static inline void qos_puts(const char* s){
    (void)qos_syscall1(SYS_PUTS, (unsigned long)s);
}

static inline void qos_sleep(unsigned int ms){
    (void)qos_syscall1(SYS_SLEEP, (unsigned long)ms);
}

static inline void qos_fb_clear(unsigned int color){
    (void)qos_syscall1(SYS_FB_CLEAR, (unsigned long)color);
}

static inline unsigned int qos_get_screen_width(void){
    return (unsigned int)qos_syscall0(SYS_FB_GET_WIDTH);
}

static inline unsigned int qos_get_screen_height(void){
    return (unsigned int)qos_syscall0(SYS_FB_GET_HEIGHT);
}

static inline void qos_fb_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color){
    (void)qos_syscall5(SYS_FB_RECT, (unsigned long)x, (unsigned long)y, (unsigned long)w, (unsigned long)h, (unsigned long)color);
}

static inline void qos_fb_present(void){
    (void)qos_syscall0(SYS_FB_PRESENT);
}

static inline int qos_try_getc(void){
    return (int)qos_syscall0(SYS_TRY_GETC);
}

static inline int qos_run_program(void){
    return (int)qos_syscall0(SYS_RUN_PROGRAM);
}

static inline void qos_net_dump_stats(void){
    (void)qos_syscall0(SYS_NET_DUMP_STATS);
}

static inline int qos_net_send_test_frame(void){
    return (int)qos_syscall0(SYS_NET_SEND_TEST_FRAME);
}

static inline int qos_net_poll(void){
    return (int)qos_syscall0(SYS_NET_POLL);
}

static inline int qos_net_recv_raw(unsigned char* out, unsigned int out_cap){
    return (int)qos_syscall2(SYS_NET_RECV_RAW, (unsigned long)out, (unsigned long)out_cap);
}

static inline int qos_net_send_raw(const unsigned char* frame, unsigned int len){
    return (int)qos_syscall2(SYS_NET_SEND_RAW, (unsigned long)frame, (unsigned long)len);
}

static inline void qos_usb_dump_info(void){
    (void)qos_syscall0(SYS_USB_DUMP_INFO);
}

static inline int qos_net_ping_gateway(unsigned int timeout_ms){
    return (int)qos_syscall1(SYS_NET_PING_GATEWAY, (unsigned long)timeout_ms);
}

static inline int qos_net_udp_send_probe(void){
    return (int)qos_syscall0(SYS_NET_UDP_SEND_PROBE);
}

static inline int qos_net_udp_recv(udp_meta_t* meta, unsigned char* out, unsigned int out_cap){
    return (int)qos_syscall3(SYS_NET_UDP_RECV, (unsigned long)meta, (unsigned long)out, (unsigned long)out_cap);
}

static inline int qos_net_udp_send(const unsigned char dst_ip[4],
                                   unsigned short src_port,
                                   unsigned short dst_port,
                                   const unsigned char* data,
                                   unsigned int len){
    return (int)qos_syscall5(SYS_NET_UDP_SEND,
                             (unsigned long)dst_ip,
                             (unsigned long)src_port,
                             (unsigned long)dst_port,
                             (unsigned long)data,
                             (unsigned long)len);
}

static inline int qos_net_tcp_http_get(const unsigned char dst_ip[4],
                                       const char* host,
                                       const char* path,
                                       unsigned char* out,
                                       unsigned int out_cap){
    return (int)qos_syscall5(SYS_NET_TCP_HTTP_GET,
                             (unsigned long)dst_ip,
                             (unsigned long)host,
                             (unsigned long)path,
                             (unsigned long)out,
                             (unsigned long)out_cap);
}

static inline int qos_socket(int domain, int type, int protocol){
    return (int)qos_syscall3(SYS_SOCKET_CREATE,
                             (unsigned long)domain,
                             (unsigned long)type,
                             (unsigned long)protocol);
}

static inline int qos_connect(int fd, const qos_sockaddr_in_t* addr, unsigned int addr_len){
    return (int)qos_syscall3(SYS_SOCKET_CONNECT,
                             (unsigned long)fd,
                             (unsigned long)addr,
                             (unsigned long)addr_len);
}

static inline int qos_send(int fd, const void* buf, unsigned int len, unsigned int flags){
    return (int)qos_syscall4(SYS_SOCKET_SEND,
                             (unsigned long)fd,
                             (unsigned long)buf,
                             (unsigned long)len,
                             (unsigned long)flags);
}

static inline int qos_recv(int fd, void* out, unsigned int out_cap, unsigned int timeout_ms){
    return (int)qos_syscall4(SYS_SOCKET_RECV,
                             (unsigned long)fd,
                             (unsigned long)out,
                             (unsigned long)out_cap,
                             (unsigned long)timeout_ms);
}

static inline int qos_close(int fd){
    return (int)qos_syscall1(SYS_SOCKET_CLOSE, (unsigned long)fd);
}

static inline int qos_socket_setopt(int fd, int opt, unsigned int value){
    return (int)qos_syscall3(SYS_SOCKET_SETOPT,
                             (unsigned long)fd,
                             (unsigned long)opt,
                             (unsigned long)value);
}

static inline int qos_socket_set_nonblocking(int fd, int enabled){
    return qos_socket_setopt(fd, QOS_SOCKOPT_NONBLOCK, (unsigned int)(enabled ? 1u : 0u));
}

static inline int qos_socket_set_recv_timeout(int fd, unsigned int timeout_ms){
    return qos_socket_setopt(fd, QOS_SOCKOPT_RCVTIMEO_MS, timeout_ms);
}

__attribute__((noreturn))
static inline void qos_exit(int code){
    (void)qos_syscall1(SYS_EXIT, (unsigned long)code);
    while (1){}
}
#endif

#endif
