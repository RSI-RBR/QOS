#ifndef SYSCALL_H
#define SYSCALL_H

#include "udp.h"
#include "socket.h"
#include "tls_session.h"
#include "cyw43.h"
#include "input_event.h"
#include "gpu2d.h"
#include "v3d.h"

#ifndef QOS_TERM_OUTPUT_UART
#define QOS_TERM_OUTPUT_UART 1u
#endif
#ifndef QOS_TERM_OUTPUT_FB
#define QOS_TERM_OUTPUT_FB   2u
#endif

#define QOS_PROC_DEAD     0
#define QOS_PROC_READY    1
#define QOS_PROC_RUNNING  2
#define QOS_PROC_SLEEPING 3
#define QOS_PROC_REAPING  4

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
    SYS_SOCKET_SETOPT = 27,
    SYS_RUN_PROGRAM_NAMED = 28,
    SYS_TTY_SET_OWNER = 29,
    SYS_TTY_RELEASE = 30,
    SYS_TTY_GET_OWNER = 31,
    SYS_PROCESS_DUMP = 32,
    SYS_GETPID = 33,
    SYS_GET_TICKS = 34,
    SYS_GET_COUNTER_HZ = 35,
    SYS_GET_COUNTER_CYCLES = 36,
    SYS_TTY_CLAIM_SELF = 37,
    SYS_TLS_OPEN = 38,
    SYS_TLS_CLOSE = 39,
    SYS_TLS_GET_LOCAL_PUBLIC = 40,
    SYS_TLS_SET_PEER_PUBLIC = 41,
    SYS_TLS_BUILD_CLIENT_HELLO = 42,
    SYS_TLS_PROCESS_SERVER_HELLO = 43,
    SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO = 44,
    SYS_TLS_RECORD_ENCRYPT = 45,
    SYS_TLS_RECORD_DECRYPT = 46,
    SYS_TLS_IS_READY = 47,
    SYS_REMOTE_LOGIN_STATS = 48,
    SYS_NET_GET_LOCAL_IP = 49,
    SYS_NET_SET_LOCAL_IP = 50,
    SYS_WIFI_INIT = 51,
    SYS_WIFI_LOAD_FW = 52,
    SYS_WIFI_UP = 53,
    SYS_WIFI_DOWN = 54,
    SYS_WIFI_SCAN = 55,
    SYS_WIFI_JOIN = 56,
    SYS_WIFI_DUMP_STATUS = 57,
    SYS_WIFI_GET_VERSION = 58,
    SYS_NET_GET_GATEWAY_IP = 59,
    SYS_NET_SET_GATEWAY_IP = 60,
    SYS_AUTH_IS_READY = 61,
    SYS_AUTH_GET_USERNAME = 62,
    SYS_AUTH_VERIFY_PASSWORD = 63,
    SYS_REMOTE_LOGIN_STATE = 64,
    SYS_TRY_GETC_EX = 65,
    SYS_TERM_GET_ACTIVE = 66,
    SYS_TERM_SWITCH = 67,
    SYS_TERM_CLEAR = 68,
    SYS_TERM_GET_OUTPUT = 69,
    SYS_TERM_SET_OUTPUT = 70,
    SYS_DMA_SET_ENABLED = 71,
    SYS_DMA_STATUS = 72,
    SYS_DMA_LAST_CS = 73,
    SYS_DMA_LAST_DEBUG = 74,
    SYS_SECURITY_LOG_DUMP = 75,
    SYS_DISPLAY_CREATE_GRAPHICS = 76,
    SYS_DISPLAY_SWITCH_GRAPHICS = 77,
    SYS_DISPLAY_SWITCH_SESSION = 78,
    SYS_PROCESS_KILL = 79,
    SYS_PROCESS_STATE = 80,
    SYS_GET_TIME_US = 81,
    SYS_GET_TIME_NS = 82,
    SYS_FB_BLIT_RGBA = 83,
    SYS_INPUT_POLL_EVENT = 84,
    SYS_FILE_READ_BMP = 85,
    SYS_FILE_PROFILE_RESET = 86,
    SYS_FILE_PROFILE_DUMP = 87,
    SYS_DMA_TRANSFER_COUNT = 88,
    SYS_DMA_LAST_BYTES = 89,
    SYS_DMA_LAST_CLEAN_US = 90,
    SYS_DMA_LAST_WAIT_US = 91,
    SYS_DMA_LAST_TOTAL_US = 92,
    SYS_TERM_SET_INPUT_LINE = 93,
    SYS_TERM_CLEAR_INPUT_LINE = 94,
    SYS_GPU_SET_ENABLED = 95,
    SYS_GPU_STATUS = 96,
    SYS_GPU_FLIP_COUNT = 97,
    SYS_DISPLAY_PROFILE_RESET = 98,
    SYS_DISPLAY_PROFILE_DUMP = 99,
    SYS_FB_BLIT_NATIVE = 100,
    SYS_FB_ATTACH_BUFFER = 101,
    SYS_FB_DIRECT_ACQUIRE = 102,
    SYS_FB_DIRECT_PRESENT = 103,
    SYS_SYSTEM_STATUS = 104,
    SYS_SYSTEM_SET_CLOCK = 105,
    SYS_GPU2D_STATUS = 106,
    SYS_GPU2D_BLIT_RGBA = 107,
    SYS_GPU2D_BLIT_COUNT = 108,
    SYS_GPU2D_FALLBACK_COUNT = 109,
    SYS_GPU2D_UNSUPPORTED_COUNT = 110,
    SYS_V3D_PROBE = 111,
    SYS_V3D_STATUS = 112,
    SYS_V3D_NOOP = 113,
    SYS_V3D_CLEAR = 114,
    SYS_GPU2D_CLEAR = 115,
    SYS_GPU2D_CLEAR_COUNT = 116,
    SYS_GPU2D_FILL_RECT = 117,
    SYS_GPU2D_FILL_COUNT = 118,
    SYS_GPU2D_TEXTURE_UPLOAD = 119,
    SYS_GPU2D_TEXTURE_FREE = 120,
    SYS_GPU2D_TEXTURE_COUNT = 121,
    SYS_GPU2D_TEXTURE_UPLOAD_COUNT = 122,
    SYS_GPU2D_TEXTURE_FREE_COUNT = 123,
    SYS_GPU2D_TEXTURE_BYTES = 124,
    SYS_FB_DIRECT_GET_DRAW = 125,
    SYS_PROCESS_LOG_READ = 126
};

#define QOS_SYSTEM_STATUS_TEMP_OK       0x01u
#define QOS_SYSTEM_STATUS_ARM_CLOCK_OK  0x02u
#define QOS_SYSTEM_STATUS_CORE_CLOCK_OK 0x04u
#define QOS_SYSTEM_STATUS_THROTTLE_OK   0x08u
#define QOS_SYSTEM_STATUS_V3D_CLOCK_OK  0x10u

typedef struct {
    unsigned int* pixels;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int page;
} qos_fb_direct_info_t;

typedef struct {
    unsigned int ok_mask;
    unsigned int temp_millic;
    unsigned int arm_hz;
    unsigned int core_hz;
    unsigned int v3d_hz;
    unsigned int throttled_flags;
} qos_system_status_t;

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

static inline int qos_term_set_input_line(const char* s){
    return (int)qos_syscall1(SYS_TERM_SET_INPUT_LINE, (unsigned long)s);
}

static inline int qos_term_clear_input_line(void){
    return (int)qos_syscall0(SYS_TERM_CLEAR_INPUT_LINE);
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

static inline int qos_fb_present(void){
    return (int)qos_syscall0(SYS_FB_PRESENT);
}

static inline int qos_fb_blit_rgba(unsigned int x,
                                   unsigned int y,
                                   unsigned int w,
                                   unsigned int h,
                                   const unsigned char* rgba){
    return (int)qos_syscall5(SYS_FB_BLIT_RGBA,
                             (unsigned long)x,
                             (unsigned long)y,
                             (unsigned long)w,
                             (unsigned long)h,
                             (unsigned long)rgba);
}

static inline int qos_fb_blit_native(unsigned int x,
                                     unsigned int y,
                                     unsigned int w,
                                     unsigned int h,
                                     const unsigned int* pixels){
    return (int)qos_syscall5(SYS_FB_BLIT_NATIVE,
                             (unsigned long)x,
                             (unsigned long)y,
                             (unsigned long)w,
                             (unsigned long)h,
                             (unsigned long)pixels);
}

static inline int qos_fb_attach_buffer(const unsigned int* pixels,
                                       unsigned int w,
                                       unsigned int h,
                                       unsigned int pitch){
    return (int)qos_syscall5(SYS_FB_ATTACH_BUFFER,
                             (unsigned long)pixels,
                             (unsigned long)w,
                             (unsigned long)h,
                             (unsigned long)pitch,
                             0ul);
}

static inline int qos_fb_direct_acquire(qos_fb_direct_info_t* out_info){
    return (int)qos_syscall1(SYS_FB_DIRECT_ACQUIRE, (unsigned long)out_info);
}

static inline int qos_fb_direct_present(qos_fb_direct_info_t* out_info){
    return (int)qos_syscall1(SYS_FB_DIRECT_PRESENT, (unsigned long)out_info);
}

static inline int qos_fb_direct_get_draw(qos_fb_direct_info_t* out_info){
    return (int)qos_syscall1(SYS_FB_DIRECT_GET_DRAW, (unsigned long)out_info);
}

static inline int qos_try_getc(void){
    return (int)qos_syscall0(SYS_TRY_GETC);
}

#define QOS_INPUT_SRC_UART   1u
#define QOS_INPUT_SRC_REMOTE 2u
#define QOS_INPUT_SRC_USB    3u

typedef struct {
    int ch;
    unsigned int source;
} qos_input_event_t;

static inline int qos_try_getc_ex(qos_input_event_t* out_ev){
    unsigned long v = qos_syscall0(SYS_TRY_GETC_EX);
    if ((long)v < 0){
        return -1;
    }
    if (out_ev){
        out_ev->ch = (int)(v & 0xFFu);
        out_ev->source = (unsigned int)((v >> 8) & 0xFFu);
    }
    return (int)(v & 0xFFu);
}

static inline int qos_poll_event(qos_event_t* out_ev){
    return (int)qos_syscall1(SYS_INPUT_POLL_EVENT, (unsigned long)out_ev);
}

static inline int qos_file_read_bmp(const char* relative_path, unsigned char* out, unsigned int out_cap){
    return (int)qos_syscall3(SYS_FILE_READ_BMP,
                             (unsigned long)relative_path,
                             (unsigned long)out,
                             (unsigned long)out_cap);
}

static inline void qos_file_profile_reset(void){
    (void)qos_syscall0(SYS_FILE_PROFILE_RESET);
}

static inline void qos_file_profile_dump(void){
    (void)qos_syscall0(SYS_FILE_PROFILE_DUMP);
}

static inline int qos_run_program(void){
    return (int)qos_syscall0(SYS_RUN_PROGRAM);
}

static inline int qos_run_program_named(const char* fat_name_83){
    return (int)qos_syscall1(SYS_RUN_PROGRAM_NAMED, (unsigned long)fat_name_83);
}

static inline int qos_display_create_graphics(int pid){
    return (int)qos_syscall1(SYS_DISPLAY_CREATE_GRAPHICS, (unsigned long)pid);
}

static inline int qos_display_switch_graphics(int pid){
    return (int)qos_syscall1(SYS_DISPLAY_SWITCH_GRAPHICS, (unsigned long)pid);
}

static inline int qos_display_switch_session(unsigned int session_id){
    return (int)qos_syscall1(SYS_DISPLAY_SWITCH_SESSION, (unsigned long)session_id);
}

static inline int qos_process_kill(int pid){
    return (int)qos_syscall1(SYS_PROCESS_KILL, (unsigned long)pid);
}

static inline int qos_process_state(int pid){
    return (int)qos_syscall1(SYS_PROCESS_STATE, (unsigned long)pid);
}

static inline int qos_process_log_read(int pid, char* out, unsigned int out_cap){
    return (int)qos_syscall3(SYS_PROCESS_LOG_READ,
                             (unsigned long)pid,
                             (unsigned long)out,
                             (unsigned long)out_cap);
}

static inline int qos_tty_set_owner(int pid){
    return (int)qos_syscall1(SYS_TTY_SET_OWNER, (unsigned long)pid);
}

static inline int qos_tty_release(void){
    return (int)qos_syscall0(SYS_TTY_RELEASE);
}

static inline int qos_tty_get_owner(void){
    return (int)qos_syscall0(SYS_TTY_GET_OWNER);
}

static inline int qos_tty_claim_self(void){
    return (int)qos_syscall0(SYS_TTY_CLAIM_SELF);
}

static inline int qos_term_get_active(void){
    return (int)qos_syscall0(SYS_TERM_GET_ACTIVE);
}

static inline int qos_term_switch(int id){
    return (int)qos_syscall1(SYS_TERM_SWITCH, (unsigned long)id);
}

static inline void qos_term_clear(void){
    (void)qos_syscall0(SYS_TERM_CLEAR);
}

static inline unsigned int qos_term_get_output(void){
    return (unsigned int)qos_syscall0(SYS_TERM_GET_OUTPUT);
}

static inline int qos_term_set_output(unsigned int flags){
    return (int)qos_syscall1(SYS_TERM_SET_OUTPUT, (unsigned long)flags);
}

static inline int qos_dma_set_enabled(int enabled){
    return (int)qos_syscall1(SYS_DMA_SET_ENABLED, (unsigned long)(enabled ? 1u : 0u));
}

static inline unsigned int qos_dma_status(void){
    return (unsigned int)qos_syscall0(SYS_DMA_STATUS);
}

static inline unsigned int qos_dma_last_cs(void){
    return (unsigned int)qos_syscall0(SYS_DMA_LAST_CS);
}

static inline unsigned int qos_dma_last_debug(void){
    return (unsigned int)qos_syscall0(SYS_DMA_LAST_DEBUG);
}

static inline unsigned int qos_dma_transfer_count(void){
    return (unsigned int)qos_syscall0(SYS_DMA_TRANSFER_COUNT);
}

static inline unsigned int qos_dma_last_bytes(void){
    return (unsigned int)qos_syscall0(SYS_DMA_LAST_BYTES);
}

static inline unsigned int qos_dma_last_clean_us(void){
    return (unsigned int)qos_syscall0(SYS_DMA_LAST_CLEAN_US);
}

static inline unsigned int qos_dma_last_wait_us(void){
    return (unsigned int)qos_syscall0(SYS_DMA_LAST_WAIT_US);
}

static inline unsigned int qos_dma_last_total_us(void){
    return (unsigned int)qos_syscall0(SYS_DMA_LAST_TOTAL_US);
}

static inline int qos_gpu_set_enabled(int enabled){
    return (int)qos_syscall1(SYS_GPU_SET_ENABLED, (unsigned long)(enabled ? 1u : 0u));
}

static inline unsigned int qos_gpu_status(void){
    return (unsigned int)qos_syscall0(SYS_GPU_STATUS);
}

static inline unsigned int qos_gpu_flip_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU_FLIP_COUNT);
}

static inline int qos_system_status(qos_system_status_t* out_status){
    return (int)qos_syscall1(SYS_SYSTEM_STATUS, (unsigned long)out_status);
}

static inline int qos_system_set_clock(unsigned int clock_id, unsigned int hz){
    return (int)qos_syscall2(SYS_SYSTEM_SET_CLOCK,
                             (unsigned long)clock_id,
                             (unsigned long)hz);
}

static inline unsigned int qos_gpu2d_status(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_STATUS);
}

static inline int qos_gpu2d_blit_rgba(const qos_gpu2d_blit_t* blit){
    return (int)qos_syscall1(SYS_GPU2D_BLIT_RGBA, (unsigned long)blit);
}

static inline int qos_gpu2d_clear(unsigned int color){
    return (int)qos_syscall1(SYS_GPU2D_CLEAR, (unsigned long)color);
}

static inline int qos_gpu2d_fill_rect(unsigned int x,
                                      unsigned int y,
                                      unsigned int w,
                                      unsigned int h,
                                      unsigned int color){
    return (int)qos_syscall5(SYS_GPU2D_FILL_RECT,
                             (unsigned long)x,
                             (unsigned long)y,
                             (unsigned long)w,
                             (unsigned long)h,
                             (unsigned long)color);
}

static inline unsigned int qos_gpu2d_blit_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_BLIT_COUNT);
}

static inline unsigned int qos_gpu2d_clear_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_CLEAR_COUNT);
}

static inline unsigned int qos_gpu2d_fill_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_FILL_COUNT);
}

static inline int qos_gpu2d_texture_upload(qos_gpu2d_texture_upload_t* req){
    return (int)qos_syscall1(SYS_GPU2D_TEXTURE_UPLOAD, (unsigned long)req);
}

static inline int qos_gpu2d_texture_free(unsigned int texture_id){
    return (int)qos_syscall1(SYS_GPU2D_TEXTURE_FREE, (unsigned long)texture_id);
}

static inline unsigned int qos_gpu2d_texture_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_TEXTURE_COUNT);
}

static inline unsigned int qos_gpu2d_texture_upload_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_TEXTURE_UPLOAD_COUNT);
}

static inline unsigned int qos_gpu2d_texture_free_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_TEXTURE_FREE_COUNT);
}

static inline unsigned int qos_gpu2d_texture_bytes(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_TEXTURE_BYTES);
}

static inline unsigned int qos_gpu2d_fallback_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_FALLBACK_COUNT);
}

static inline unsigned int qos_gpu2d_unsupported_count(void){
    return (unsigned int)qos_syscall0(SYS_GPU2D_UNSUPPORTED_COUNT);
}

static inline int qos_v3d_probe(qos_v3d_status_t* out_status){
    return (int)qos_syscall1(SYS_V3D_PROBE, (unsigned long)out_status);
}

static inline int qos_v3d_status(qos_v3d_status_t* out_status){
    return (int)qos_syscall1(SYS_V3D_STATUS, (unsigned long)out_status);
}

static inline int qos_v3d_noop(unsigned int thread, qos_v3d_status_t* out_status){
    return (int)qos_syscall2(SYS_V3D_NOOP,
                             (unsigned long)thread,
                             (unsigned long)out_status);
}

static inline int qos_v3d_clear(unsigned int rgba, qos_v3d_status_t* out_status){
    return (int)qos_syscall2(SYS_V3D_CLEAR,
                             (unsigned long)rgba,
                             (unsigned long)out_status);
}

static inline void qos_display_profile_reset(void){
    (void)qos_syscall0(SYS_DISPLAY_PROFILE_RESET);
}

static inline void qos_display_profile_dump(void){
    (void)qos_syscall0(SYS_DISPLAY_PROFILE_DUMP);
}

static inline void qos_security_log_dump(void){
    (void)qos_syscall0(SYS_SECURITY_LOG_DUMP);
}

static inline void qos_process_dump(void){
    (void)qos_syscall0(SYS_PROCESS_DUMP);
}

static inline int qos_getpid(void){
    return (int)qos_syscall0(SYS_GETPID);
}

static inline unsigned long qos_get_ticks(void){
    return qos_syscall0(SYS_GET_TICKS);
}

static inline unsigned long qos_get_counter_hz(void){
    unsigned long hz;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz;
}

static inline unsigned long qos_get_counter_cycles(void){
    unsigned long cycles;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cycles));
    return cycles;
}

static inline unsigned long long qos_cycles_to_us(unsigned long long cycles, unsigned long long hz){
    if (hz == 0ull){
        return 0ull;
    }
    {
        // Keep userspace helper 64-bit only in freestanding builds to avoid
        // pulling compiler runtime helpers such as __udivti3.
        unsigned long long whole = (cycles / hz) * 1000000ull;
        unsigned long long rem = cycles % hz;
        return whole + ((rem * 1000000ull) / hz);
    }
}

static inline unsigned long long qos_cycles_to_ns(unsigned long long cycles, unsigned long long hz){
    if (hz == 0ull){
        return 0ull;
    }
    {
        // Keep userspace helper 64-bit only in freestanding builds to avoid
        // pulling compiler runtime helpers such as __udivti3.
        unsigned long long whole = (cycles / hz) * 1000000000ull;
        unsigned long long rem = cycles % hz;
        return whole + ((rem * 1000000000ull) / hz);
    }
}

static inline unsigned long long qos_get_time_us(void){
    return qos_cycles_to_us((unsigned long long)qos_get_counter_cycles(),
                            (unsigned long long)qos_get_counter_hz());
}

static inline unsigned long long qos_get_time_ns(void){
    return qos_cycles_to_ns((unsigned long long)qos_get_counter_cycles(),
                            (unsigned long long)qos_get_counter_hz());
}

static inline void qos_sleep_us(unsigned long long us){
    unsigned long long start;
    unsigned long long deadline;
    if (us == 0ull){
        return;
    }
    start = qos_get_time_us();
    deadline = start + us;
    // Use cooperative sleep for the coarse portion, then busy-wait the tail.
    if (us >= 2000ull){
        unsigned int coarse_ms = (unsigned int)((us - 1000ull) / 1000ull);
        if (coarse_ms > 0u){
            qos_sleep(coarse_ms);
        }
    }
    while ((long long)(qos_get_time_us() - deadline) < 0){
        asm volatile("yield");
    }
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

static inline int qos_tls_open(int role){
    return (int)qos_syscall1(SYS_TLS_OPEN, (unsigned long)role);
}

static inline int qos_tls_close(int tls_id){
    return (int)qos_syscall1(SYS_TLS_CLOSE, (unsigned long)tls_id);
}

static inline int qos_tls_get_local_public(int tls_id, unsigned char out_public[32]){
    return (int)qos_syscall2(SYS_TLS_GET_LOCAL_PUBLIC,
                             (unsigned long)tls_id,
                             (unsigned long)out_public);
}

static inline int qos_tls_set_peer_public(int tls_id, const unsigned char peer_public[32]){
    return (int)qos_syscall2(SYS_TLS_SET_PEER_PUBLIC,
                             (unsigned long)tls_id,
                             (unsigned long)peer_public);
}

static inline int qos_tls_build_client_hello(int tls_id, unsigned char* out, unsigned int out_cap){
    return (int)qos_syscall3(SYS_TLS_BUILD_CLIENT_HELLO,
                             (unsigned long)tls_id,
                             (unsigned long)out,
                             (unsigned long)out_cap);
}

static inline int qos_tls_process_server_hello(int tls_id, const unsigned char* in, unsigned int in_len){
    return (int)qos_syscall3(SYS_TLS_PROCESS_SERVER_HELLO,
                             (unsigned long)tls_id,
                             (unsigned long)in,
                             (unsigned long)in_len);
}

static inline int qos_tls_process_client_hello_build_server_hello(int tls_id,
                                                                   const unsigned char* in, unsigned int in_len,
                                                                   unsigned char* out, unsigned int out_cap){
    return (int)qos_syscall5(SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO,
                             (unsigned long)tls_id,
                             (unsigned long)in,
                             (unsigned long)in_len,
                             (unsigned long)out,
                             (unsigned long)out_cap);
}

static inline int qos_tls_record_encrypt(int tls_id, qos_tls_record_io_t* io){
    return (int)qos_syscall2(SYS_TLS_RECORD_ENCRYPT,
                             (unsigned long)tls_id,
                             (unsigned long)io);
}

static inline int qos_tls_record_decrypt(int tls_id, qos_tls_record_io_t* io){
    return (int)qos_syscall2(SYS_TLS_RECORD_DECRYPT,
                             (unsigned long)tls_id,
                             (unsigned long)io);
}

static inline int qos_tls_is_ready(int tls_id){
    return (int)qos_syscall1(SYS_TLS_IS_READY, (unsigned long)tls_id);
}

static inline void qos_remote_login_dump_stats(void){
    (void)qos_syscall0(SYS_REMOTE_LOGIN_STATS);
}

#define QOS_RLOGIN_STATE_ENABLED      (1u << 0)
#define QOS_RLOGIN_STATE_ACTIVE       (1u << 1)
#define QOS_RLOGIN_STATE_AUTHED       (1u << 2)
#define QOS_RLOGIN_STATE_TTY_ATTACHED (1u << 3)

static inline unsigned int qos_remote_login_state(void){
    return (unsigned int)qos_syscall0(SYS_REMOTE_LOGIN_STATE);
}

static inline int qos_net_get_local_ip(unsigned char out_ip[4]){
    return (int)qos_syscall1(SYS_NET_GET_LOCAL_IP, (unsigned long)out_ip);
}

static inline int qos_net_set_local_ip(const unsigned char ip[4]){
    return (int)qos_syscall1(SYS_NET_SET_LOCAL_IP, (unsigned long)ip);
}

static inline int qos_net_get_gateway_ip(unsigned char out_ip[4]){
    return (int)qos_syscall1(SYS_NET_GET_GATEWAY_IP, (unsigned long)out_ip);
}

static inline int qos_net_set_gateway_ip(const unsigned char ip[4]){
    return (int)qos_syscall1(SYS_NET_SET_GATEWAY_IP, (unsigned long)ip);
}

static inline int qos_auth_is_ready(void){
    return (int)qos_syscall0(SYS_AUTH_IS_READY);
}

static inline int qos_auth_get_username(char* out, unsigned int out_cap){
    return (int)qos_syscall2(SYS_AUTH_GET_USERNAME,
                             (unsigned long)out,
                             (unsigned long)out_cap);
}

static inline int qos_auth_verify_password(const char* username, const char* password){
    return (int)qos_syscall2(SYS_AUTH_VERIFY_PASSWORD,
                             (unsigned long)username,
                             (unsigned long)password);
}

static inline int qos_wifi_init(void){
    return (int)qos_syscall0(SYS_WIFI_INIT);
}

static inline int qos_wifi_load_fw(const char* fw_bin_83, const char* nvram_txt_83){
    return (int)qos_syscall2(SYS_WIFI_LOAD_FW, (unsigned long)fw_bin_83, (unsigned long)nvram_txt_83);
}

static inline int qos_wifi_up(void){
    return (int)qos_syscall0(SYS_WIFI_UP);
}

static inline int qos_wifi_down(void){
    return (int)qos_syscall0(SYS_WIFI_DOWN);
}

static inline int qos_wifi_scan(cyw43_scan_result_t* out, unsigned int cap){
    return (int)qos_syscall2(SYS_WIFI_SCAN, (unsigned long)out, (unsigned long)cap);
}

static inline int qos_wifi_join(const char* ssid, const char* password){
    return (int)qos_syscall2(SYS_WIFI_JOIN, (unsigned long)ssid, (unsigned long)password);
}

static inline void qos_wifi_dump_status(void){
    (void)qos_syscall0(SYS_WIFI_DUMP_STATUS);
}

static inline int qos_wifi_get_version(char* out, unsigned int out_cap){
    return (int)qos_syscall2(SYS_WIFI_GET_VERSION, (unsigned long)out, (unsigned long)out_cap);
}

__attribute__((noreturn))
static inline void qos_exit(int code){
    (void)qos_syscall1(SYS_EXIT, (unsigned long)code);
    while (1){}
}
#endif

#endif
