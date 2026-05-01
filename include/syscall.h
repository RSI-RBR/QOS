#ifndef SYSCALL_H
#define SYSCALL_H

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
    SYS_RUN_PROGRAM = 10
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
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");
    return x0;
}

static inline unsigned long qos_syscall1(unsigned long n, unsigned long a0){
    register unsigned long x0 asm("x0") = a0;
    register unsigned long x8 asm("x8") = n;
    asm volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");
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
        : "x2", "x3", "x4", "x5", "x6", "x7", "memory");
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
        : "x5", "x6", "x7", "memory");
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

__attribute__((noreturn))
static inline void qos_exit(int code){
    (void)qos_syscall1(SYS_EXIT, (unsigned long)code);
    while (1){}
}
#endif

#endif
