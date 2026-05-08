#include "syscall.h"
#include "uart.h"
#include "process.h"
#include "framebuffer.h"
#include "timer.h"
#include "loader.h"
#include "memory.h"
#include "net.h"
#include "net_proto.h"
#include "udp.h"
#include "tcp.h"
#include "usb_host.h"
#include "interrupt.h"
#include "socket.h"
#include "console.h"
#include "tls_session.h"
#include "remote_login.h"
#include "cyw43.h"
#include "auth.h"
#include "trust.h"

#define ESR_EC_SHIFT 26
#define ESR_EC_MASK   0x3FUL
#define ESR_EC_SVC64  0x15UL
#define USER_CSTR_MAX 256u
#define USER_PASS_MAX 128u
#define USER_IO_MAX   16384u
#define USER_WIFI_SCAN_MAX 64u

// Trap frame layout in vectors.S
#define TF_X0   0
#define TF_X1   1
#define TF_X2   2
#define TF_X3   3
#define TF_X4   4
#define TF_X8   8

static unsigned long clamp_puts_len(const char* s){
    unsigned long max = 1024;
    unsigned long n = 0;
    while (n < max && s[n]){
        n++;
    }
    return n;
}

static int copy_cstr_out(char* out, unsigned int out_cap, const char* in){
    unsigned int i = 0;
    if (!out || out_cap == 0u || !in){
        return -1;
    }
    while (i + 1u < out_cap && in[i]){
        out[i] = in[i];
        i++;
    }
    out[i] = 0;
    return 0;
}

static unsigned int clamp_u32(unsigned int v, unsigned int max){
    return (v > max) ? max : v;
}

static int copy_cstr_from_user_bound(char* out, unsigned int out_cap, const char* user_in){
    return process_copy_cstr_from_user(out, out_cap, user_in);
}

static unsigned int cstr_bytes_with_nul(const char* s, unsigned int cap){
    unsigned int n = 0;
    if (!s || cap == 0u){
        return 0u;
    }
    while (n + 1u < cap && s[n]){
        n++;
    }
    return n + 1u;
}

static void syscall_poll_background_io(void){
    static unsigned long next_poll_tick = 0;
    unsigned long now = system_ticks;

    if ((long)(now - next_poll_tick) < 0){
        return;
    }
    // Keep interactive shell and remote-login latency low.
    next_poll_tick = now + 2u;
    (void)net_poll();
    remote_login_poll();
}

static void syscall_write_puts(int pid, const char* s){
    if (!s){
        return;
    }
    unsigned long n = clamp_puts_len(s);
    for (unsigned long i = 0; i < n; i++){
        if (pid >= 0 && console_get_owner() == pid){
            remote_login_on_tty_output_char(s[i]);
        }
        if (s[i] == '\n'){
            uart_send('\r');
        }
        uart_send(s[i]);
    }
}

static void syscall_dump_usb_info(void){
    usb_root_device_info_t info;
    if (usb_host_get_root_device_info(&info) != 0){
        syscall_write_puts(-1, "USB root: not enumerated\n");
        return;
    }

    syscall_write_puts(-1, "USB root addr=");
    uart_puthex(info.address);
    syscall_write_puts(-1, " vid=");
    uart_puthex(info.vid);
    syscall_write_puts(-1, " pid=");
    uart_puthex(info.pid);
    syscall_write_puts(-1, " class=");
    uart_puthex(info.dev_class);
    syscall_write_puts(-1, " cfg=");
    uart_puthex(info.config_value);
    syscall_write_puts(-1, info.configured ? " (set)\n" : " (not set)\n");

    if (info.child_present){
        syscall_write_puts(-1, "USB child addr=");
        uart_puthex(info.child_address);
        syscall_write_puts(-1, " vid=");
        uart_puthex(info.child_vid);
        syscall_write_puts(-1, " pid=");
        uart_puthex(info.child_pid);
        syscall_write_puts(-1, " class=");
        uart_puthex(info.child_class);
        syscall_write_puts(-1, " cfg=");
        uart_puthex(info.child_config_value);
        syscall_write_puts(-1, info.child_configured ? " (set)\n" : " (not set)\n");
        syscall_write_puts(-1, "USB child bulk in=");
        uart_puthex(info.child_bulk_in_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_bulk_in_mps);
        syscall_write_puts(-1, " out=");
        uart_puthex(info.child_bulk_out_ep);
        syscall_write_puts(-1, " mps=");
        uart_puthex(info.child_bulk_out_mps);
        syscall_write_puts(-1, "\n");
    }
}

static int syscall_capability_allowed(const process_t* proc, unsigned long nr){
    unsigned int req_scope_any = 0u;
    unsigned int req_role_any = TRUST_ROLE_DEVELOPER | TRUST_ROLE_ADMIN;

    if (!proc || !proc->user_mode){
        return 1;
    }

    switch (nr){
        case SYS_PUTC:
        case SYS_PUTS:
        case SYS_SLEEP:
        case SYS_EXIT:
        case SYS_FB_CLEAR:
        case SYS_FB_GET_WIDTH:
        case SYS_FB_GET_HEIGHT:
        case SYS_FB_RECT:
        case SYS_FB_PRESENT:
        case SYS_TRY_GETC:
        case SYS_TRY_GETC_EX:
        case SYS_GETPID:
        case SYS_GET_TICKS:
        case SYS_GET_COUNTER_HZ:
        case SYS_GET_COUNTER_CYCLES:
            req_scope_any = TRUST_SCOPE_USER_APP | TRUST_SCOPE_SHELL | TRUST_SCOPE_WEB;
            break;

        case SYS_NET_DUMP_STATS:
        case SYS_NET_SEND_TEST_FRAME:
        case SYS_NET_POLL:
        case SYS_NET_RECV_RAW:
        case SYS_NET_SEND_RAW:
        case SYS_NET_PING_GATEWAY:
        case SYS_NET_UDP_SEND_PROBE:
        case SYS_NET_UDP_RECV:
        case SYS_NET_UDP_SEND:
        case SYS_NET_TCP_HTTP_GET:
        case SYS_SOCKET_CREATE:
        case SYS_SOCKET_CONNECT:
        case SYS_SOCKET_SEND:
        case SYS_SOCKET_RECV:
        case SYS_SOCKET_CLOSE:
        case SYS_SOCKET_SETOPT:
        case SYS_TLS_OPEN:
        case SYS_TLS_CLOSE:
        case SYS_TLS_GET_LOCAL_PUBLIC:
        case SYS_TLS_SET_PEER_PUBLIC:
        case SYS_TLS_BUILD_CLIENT_HELLO:
        case SYS_TLS_PROCESS_SERVER_HELLO:
        case SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO:
        case SYS_TLS_RECORD_ENCRYPT:
        case SYS_TLS_RECORD_DECRYPT:
        case SYS_TLS_IS_READY:
        case SYS_NET_GET_LOCAL_IP:
        case SYS_NET_GET_GATEWAY_IP:
            req_scope_any = TRUST_SCOPE_WEB | TRUST_SCOPE_SHELL;
            break;

        case SYS_RUN_PROGRAM:
        case SYS_RUN_PROGRAM_NAMED:
        case SYS_TTY_SET_OWNER:
        case SYS_TTY_RELEASE:
        case SYS_TTY_GET_OWNER:
        case SYS_TTY_CLAIM_SELF:
        case SYS_PROCESS_DUMP:
        case SYS_REMOTE_LOGIN_STATS:
        case SYS_NET_SET_LOCAL_IP:
        case SYS_NET_SET_GATEWAY_IP:
        case SYS_REMOTE_LOGIN_STATE:
        case SYS_AUTH_IS_READY:
        case SYS_AUTH_GET_USERNAME:
        case SYS_AUTH_VERIFY_PASSWORD:
        case SYS_WIFI_INIT:
        case SYS_WIFI_LOAD_FW:
        case SYS_WIFI_UP:
        case SYS_WIFI_DOWN:
        case SYS_WIFI_SCAN:
        case SYS_WIFI_JOIN:
        case SYS_WIFI_DUMP_STATUS:
        case SYS_WIFI_GET_VERSION:
        case SYS_USB_DUMP_INFO:
            req_scope_any = TRUST_SCOPE_SHELL;
            req_role_any = TRUST_ROLE_ADMIN;
            break;

        default:
            return 0;
    }

    if ((proc->signer_scope_mask & req_scope_any) == 0u){
        return 0;
    }
    if ((proc->signer_role_mask & req_role_any) == 0u){
        return 0;
    }
    return 1;
}

void* syscall_handle(void* frame_sp, unsigned long esr){
    unsigned long ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    unsigned long* frame = (unsigned long*)frame_sp;

    if (ec != ESR_EC_SVC64 || !frame){
        return frame_sp;
    }

    unsigned long nr = frame[TF_X8];
    process_t* caller = get_current_process();
    if (!syscall_capability_allowed(caller, nr)){
        frame[TF_X0] = (unsigned long)-1;
        return frame_sp;
    }

    switch (nr){
        case SYS_PUTC:
            if (console_get_owner() == process_current_pid()){
                remote_login_on_tty_output_char((char)frame[TF_X0]);
            }
            uart_send((char)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_PUTS:
        {
            char tmp[1025];
            tmp[0] = 0;
            if (copy_cstr_from_user_bound(tmp, sizeof(tmp), (const char*)frame[TF_X0]) == 0){
                syscall_write_puts(process_current_pid(), tmp);
            }
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_SLEEP: {
            unsigned int ms = (unsigned int)frame[TF_X0];
            frame[TF_X0] = 0;
            return process_sleep_on_frame(ms, frame_sp);
        }

        case SYS_EXIT:
            process_exit_current();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_FB_CLEAR:
            fb_clear((unsigned int)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_FB_GET_WIDTH:
            frame[TF_X0] = fb_get_width();
            return frame_sp;

        case SYS_FB_GET_HEIGHT:
            frame[TF_X0] = fb_get_height();
            return frame_sp;

        case SYS_FB_RECT:
            fb_edit_buffer_rect_fast((unsigned int)frame[TF_X0],
                                     (unsigned int)frame[TF_X1],
                                     (unsigned int)frame[TF_X2],
                                     (unsigned int)frame[TF_X3],
                                     (unsigned int)frame[TF_X4]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_FB_PRESENT:
            fb_update_buffer_pixels_fast();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_TRY_GETC: {
            char c = 0;
            syscall_poll_background_io();
            if (console_try_getc_for_pid(process_current_pid(), &c)){
                frame[TF_X0] = (unsigned long)(unsigned char)c;
            } else{
                frame[TF_X0] = (unsigned long)-1;
            }
            return frame_sp;
        }

        case SYS_TRY_GETC_EX: {
            char c = 0;
            unsigned int src = 0u;
            syscall_poll_background_io();
            if (console_try_getc_for_pid_ex(process_current_pid(), &c, &src)){
                frame[TF_X0] = ((unsigned long)(src & 0xFFu) << 8) |
                               (unsigned long)((unsigned char)c);
            } else{
                frame[TF_X0] = (unsigned long)-1;
            }
            return frame_sp;
        }

        case SYS_RUN_PROGRAM: {
            kernel_preempt_enter();
            loaded_program_t prog = load_program_from_sd();
            if (!prog.entry){
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            int pid = process_create_loaded(prog);
            if (pid < 0){
                if (prog.heap_allocated){
                    kfree_secure(prog.memory, prog.size);
                } else{
                    volatile unsigned char* m = (volatile unsigned char*)prog.memory;
                    for (unsigned long i = 0; i < prog.size; i++){
                        m[i] = 0;
                    }
                    loader_free_program_memory(prog.memory, prog.size);
                }
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kernel_preempt_exit();
            frame[TF_X0] = (unsigned long)pid;
            return frame_sp;
        }

        case SYS_RUN_PROGRAM_NAMED: {
            const char* fat_name_83 = (const char*)frame[TF_X0];
            char fat_name_local[12];
            if (!fat_name_83 || process_copy_from_user(fat_name_local, fat_name_83, 11u) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            fat_name_local[11] = 0;

            kernel_preempt_enter();
            loaded_program_t prog = load_program_from_sd_named(fat_name_local);
            if (!prog.entry){
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            int pid = process_create_loaded(prog);
            if (pid < 0){
                if (prog.heap_allocated){
                    kfree_secure(prog.memory, prog.size);
                } else{
                    volatile unsigned char* m = (volatile unsigned char*)prog.memory;
                    for (unsigned long i = 0; i < prog.size; i++){
                        m[i] = 0;
                    }
                    loader_free_program_memory(prog.memory, prog.size);
                }
                kernel_preempt_exit();
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kernel_preempt_exit();
            frame[TF_X0] = (unsigned long)pid;
            return frame_sp;
        }

        case SYS_NET_DUMP_STATS:
            (void)net_poll();
            remote_login_poll();
            net_dump_stats();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_NET_SEND_TEST_FRAME:
            frame[TF_X0] = (unsigned long)net_send_test_frame();
            return frame_sp;

        case SYS_NET_POLL:
            frame[TF_X0] = (unsigned long)net_poll();
            return frame_sp;

        case SYS_NET_RECV_RAW:
        {
            unsigned char* user_out = (unsigned char*)frame[TF_X0];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X1], NET_MAX_FRAME_SIZE);
            unsigned char kbuf[NET_MAX_FRAME_SIZE];
            int n = net_recv_raw(kbuf, cap);
            if (n > 0){
                if (process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_NET_SEND_RAW:
        {
            const unsigned char* user_frame = (const unsigned char*)frame[TF_X0];
            unsigned int len = (unsigned int)frame[TF_X1];
            if (len == 0u || len > NET_MAX_FRAME_SIZE){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned char kbuf[NET_MAX_FRAME_SIZE];
            if (process_copy_from_user(kbuf, user_frame, len) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)net_send_raw(kbuf, len);
            return frame_sp;
        }

        case SYS_USB_DUMP_INFO:
            syscall_dump_usb_info();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_NET_PING_GATEWAY:
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)net_ping_gateway((unsigned int)frame[TF_X0]);
            kernel_preempt_exit();
            return frame_sp;

        case SYS_NET_UDP_SEND_PROBE:
            frame[TF_X0] = (unsigned long)udp_send_probe_gateway();
            return frame_sp;

        case SYS_NET_UDP_RECV:
        {
            udp_meta_t kmeta;
            unsigned char kbuf[UDP_MAX_PAYLOAD];
            udp_meta_t* user_meta = (udp_meta_t*)frame[TF_X0];
            unsigned char* user_out = (unsigned char*)frame[TF_X1];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X2], UDP_MAX_PAYLOAD);
            int n = udp_recv_next(kbuf, cap, &kmeta);
            if (n > 0){
                if (process_copy_to_user(user_out, kbuf, (unsigned long)n) != 0 ||
                    process_copy_to_user(user_meta, &kmeta, sizeof(kmeta)) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }
            frame[TF_X0] = (unsigned long)n;
            return frame_sp;
        }

        case SYS_NET_UDP_SEND:
        {
            unsigned char dst_ip[4];
            unsigned int len = (unsigned int)frame[TF_X4];
            if (len > UDP_MAX_PAYLOAD){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            unsigned char kbuf[UDP_MAX_PAYLOAD];
            if (process_copy_from_user(dst_ip, (const void*)frame[TF_X0], 4u) != 0 ||
                process_copy_from_user(kbuf, (const void*)frame[TF_X3], len) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)udp_send(dst_ip,
                                                   (unsigned short)frame[TF_X1],
                                                   (unsigned short)frame[TF_X2],
                                                   kbuf,
                                                   len);
            return frame_sp;
        }

        case SYS_NET_TCP_HTTP_GET:
        {
            unsigned char dst_ip[4];
            char host[USER_CSTR_MAX];
            char path[USER_CSTR_MAX];
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X4], USER_IO_MAX);
            unsigned char* user_out = (unsigned char*)frame[TF_X3];
            unsigned char* kout = 0;

            if (out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (process_copy_from_user(dst_ip, (const void*)frame[TF_X0], 4u) != 0 ||
                copy_cstr_from_user_bound(host, sizeof(host), (const char*)frame[TF_X1]) != 0 ||
                copy_cstr_from_user_bound(path, sizeof(path), (const char*)frame[TF_X2]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (unsigned char*)kmalloc(out_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kernel_preempt_enter();
            int rc = tcp_http_get(dst_ip, host, path, kout, out_cap);
            kernel_preempt_exit();
            if (rc > 0 && process_copy_to_user(user_out, kout, (unsigned long)rc) != 0){
                rc = -1;
            }
            kfree(kout);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_SOCKET_CREATE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_create(pid,
                                                         (int)frame[TF_X0],
                                                         (int)frame[TF_X1],
                                                         (int)frame[TF_X2]);
            return frame_sp;
        }

        case SYS_SOCKET_CONNECT: {
            int pid = process_current_pid();
            qos_sockaddr_in_t addr;
            if ((unsigned int)frame[TF_X2] < (unsigned int)sizeof(addr) ||
                process_copy_from_user(&addr, (const void*)frame[TF_X1], sizeof(addr)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)ksocket_connect(pid,
                                                          (int)frame[TF_X0],
                                                          &addr,
                                                          (unsigned int)sizeof(addr));
            return frame_sp;
        }

        case SYS_SOCKET_SEND: {
            int pid = process_current_pid();
            unsigned int len = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned char* kbuf = 0;
            if (len == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kbuf = (unsigned char*)kmalloc(len);
            if (!kbuf || process_copy_from_user(kbuf, (const void*)frame[TF_X1], len) != 0){
                if (kbuf){ kfree(kbuf); }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ksocket_send(pid,
                                  (int)frame[TF_X0],
                                  kbuf,
                                  len,
                                  (unsigned int)frame[TF_X3]);
            kfree(kbuf);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_SOCKET_RECV: {
            int pid = process_current_pid();
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned char* kout = 0;
            if (out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kout = (unsigned char*)kmalloc(out_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kernel_preempt_enter();
            int rc = ksocket_recv(pid,
                                  (int)frame[TF_X0],
                                  kout,
                                  out_cap,
                                  (unsigned int)frame[TF_X3]);
            kernel_preempt_exit();
            if (rc > 0 && process_copy_to_user((void*)frame[TF_X1], kout, (unsigned long)rc) != 0){
                rc = -1;
            }
            kfree(kout);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_SOCKET_CLOSE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_close(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_SOCKET_SETOPT: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_setopt(pid,
                                                         (int)frame[TF_X0],
                                                         (int)frame[TF_X1],
                                                         (unsigned int)frame[TF_X2]);
            return frame_sp;
        }

        case SYS_TTY_SET_OWNER: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)console_set_owner(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_TTY_RELEASE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)console_release_owner(pid);
            return frame_sp;
        }

        case SYS_TTY_GET_OWNER:
            frame[TF_X0] = (unsigned long)console_get_owner();
            return frame_sp;

        case SYS_TTY_CLAIM_SELF: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)console_set_owner(pid, pid);
            return frame_sp;
        }

        case SYS_PROCESS_DUMP:
            process_dump();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_GETPID:
            frame[TF_X0] = (unsigned long)process_current_pid();
            return frame_sp;

        case SYS_GET_TICKS:
            frame[TF_X0] = system_ticks;
            return frame_sp;

        case SYS_GET_COUNTER_HZ: {
            unsigned long hz = 0;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
            frame[TF_X0] = hz;
            return frame_sp;
        }

        case SYS_GET_COUNTER_CYCLES: {
            unsigned long cyc = 0;
            asm volatile("mrs %0, cntpct_el0" : "=r"(cyc));
            frame[TF_X0] = cyc;
            return frame_sp;
        }

        case SYS_TLS_OPEN: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_open(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_TLS_CLOSE: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_close(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_TLS_GET_LOCAL_PUBLIC: {
            int pid = process_current_pid();
            unsigned char pub[32];
            int rc = ktls_get_local_public(pid, (int)frame[TF_X0], pub);
            if (rc == 0 && process_copy_to_user((void*)frame[TF_X1], pub, sizeof(pub)) != 0){
                rc = -1;
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_SET_PEER_PUBLIC: {
            int pid = process_current_pid();
            unsigned char pub[32];
            if (process_copy_from_user(pub, (const void*)frame[TF_X1], sizeof(pub)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            frame[TF_X0] = (unsigned long)ktls_set_peer_public(pid,
                                                                (int)frame[TF_X0],
                                                                pub);
            return frame_sp;
        }

        case SYS_TLS_BUILD_CLIENT_HELLO: {
            int pid = process_current_pid();
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned int out_len = 0;
            unsigned char* kout = 0;
            if (out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kout = (unsigned char*)kmalloc(out_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ktls_build_client_hello(pid,
                                             (int)frame[TF_X0],
                                             kout,
                                             out_cap,
                                             &out_len);
            if (rc == 0 && process_copy_to_user((void*)frame[TF_X1], kout, out_len) != 0){
                rc = -1;
            }
            kfree(kout);
            frame[TF_X0] = (rc == 0) ? (unsigned long)out_len : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_TLS_PROCESS_SERVER_HELLO: {
            int pid = process_current_pid();
            unsigned int in_len = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned char* kin = 0;
            if (in_len == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kin = (unsigned char*)kmalloc(in_len);
            if (!kin || process_copy_from_user(kin, (const void*)frame[TF_X1], in_len) != 0){
                if (kin){ kfree(kin); }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ktls_process_server_hello(pid,
                                               (int)frame[TF_X0],
                                               kin,
                                               in_len);
            kfree(kin);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO: {
            int pid = process_current_pid();
            unsigned int in_len = clamp_u32((unsigned int)frame[TF_X2], USER_IO_MAX);
            unsigned int out_cap = clamp_u32((unsigned int)frame[TF_X4], USER_IO_MAX);
            unsigned char* kin = 0;
            unsigned char* kout = 0;
            unsigned int out_len = 0;
            if (in_len == 0u || out_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            kin = (unsigned char*)kmalloc(in_len);
            kout = (unsigned char*)kmalloc(out_cap);
            if (!kin || !kout || process_copy_from_user(kin, (const void*)frame[TF_X1], in_len) != 0){
                if (kin){ kfree(kin); }
                if (kout){ kfree(kout); }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            int rc = ktls_process_client_hello_build_server_hello(pid,
                                                                   (int)frame[TF_X0],
                                                                   kin,
                                                                   in_len,
                                                                   kout,
                                                                   out_cap,
                                                                   &out_len);
            if (rc == 0 && process_copy_to_user((void*)frame[TF_X3], kout, out_len) != 0){
                rc = -1;
            }
            kfree(kin);
            kfree(kout);
            frame[TF_X0] = (rc == 0) ? (unsigned long)out_len : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_TLS_RECORD_ENCRYPT: {
            int pid = process_current_pid();
            qos_tls_record_io_t user_io;
            qos_tls_record_io_t kio;
            unsigned char* kin = 0;
            unsigned char* kout = 0;
            int rc = -1;

            if (process_copy_from_user(&user_io, (const void*)frame[TF_X1], sizeof(user_io)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (!user_io.out || user_io.out_cap == 0u || user_io.out_cap > USER_IO_MAX ||
                user_io.in_len > USER_IO_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            if (user_io.in_len > 0u){
                if (!user_io.in){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                kin = (unsigned char*)kmalloc(user_io.in_len);
                if (!kin || process_copy_from_user(kin, user_io.in, user_io.in_len) != 0){
                    if (kin){
                        kfree_secure(kin, user_io.in_len);
                    }
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
            }

            kout = (unsigned char*)kmalloc(user_io.out_cap);
            if (!kout){
                if (kin){
                    kfree_secure(kin, user_io.in_len);
                }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kio.inner_type = user_io.inner_type;
            kio.in = kin;
            kio.in_len = user_io.in_len;
            kio.out = kout;
            kio.out_cap = user_io.out_cap;
            kio.out_len = 0u;

            rc = ktls_record_encrypt(pid, (int)frame[TF_X0], &kio);
            if (rc > 0){
                if (kio.out_len > user_io.out_cap ||
                    process_copy_to_user(user_io.out, kout, kio.out_len) != 0){
                    rc = -1;
                }
            }

            user_io.out_len = kio.out_len;
            if (process_copy_to_user((void*)frame[TF_X1], &user_io, sizeof(user_io)) != 0){
                rc = -1;
            }

            kfree_secure(kout, user_io.out_cap);
            if (kin){
                kfree_secure(kin, user_io.in_len);
            }
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_RECORD_DECRYPT: {
            int pid = process_current_pid();
            qos_tls_record_io_t user_io;
            qos_tls_record_io_t kio;
            unsigned char* kin = 0;
            unsigned char* kout = 0;
            int rc = -1;

            if (process_copy_from_user(&user_io, (const void*)frame[TF_X1], sizeof(user_io)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (!user_io.in || !user_io.out ||
                user_io.in_len == 0u || user_io.out_cap == 0u ||
                user_io.in_len > USER_IO_MAX || user_io.out_cap > USER_IO_MAX){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kin = (unsigned char*)kmalloc(user_io.in_len);
            kout = (unsigned char*)kmalloc(user_io.out_cap);
            if (!kin || !kout ||
                process_copy_from_user(kin, user_io.in, user_io.in_len) != 0){
                if (kin){
                    kfree_secure(kin, user_io.in_len);
                }
                if (kout){
                    kfree_secure(kout, user_io.out_cap);
                }
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kio.inner_type = user_io.inner_type;
            kio.in = kin;
            kio.in_len = user_io.in_len;
            kio.out = kout;
            kio.out_cap = user_io.out_cap;
            kio.out_len = 0u;

            rc = ktls_record_decrypt(pid, (int)frame[TF_X0], &kio);
            if (rc > 0){
                if (kio.out_len > user_io.out_cap ||
                    process_copy_to_user(user_io.out, kout, kio.out_len) != 0){
                    rc = -1;
                }
            }

            user_io.inner_type = kio.inner_type;
            user_io.out_len = kio.out_len;
            if (process_copy_to_user((void*)frame[TF_X1], &user_io, sizeof(user_io)) != 0){
                rc = -1;
            }

            kfree_secure(kout, user_io.out_cap);
            kfree_secure(kin, user_io.in_len);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        case SYS_TLS_IS_READY: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_is_ready(pid, (int)frame[TF_X0]);
            return frame_sp;
        }

        case SYS_REMOTE_LOGIN_STATS:
            (void)net_poll();
            remote_login_poll();
            remote_login_dump_stats();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_REMOTE_LOGIN_STATE:
            frame[TF_X0] = (unsigned long)remote_login_state_bits();
            return frame_sp;

        case SYS_NET_GET_LOCAL_IP:
        {
            unsigned char ip[4];
            net_proto_get_local_ip(ip);
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0], ip, sizeof(ip)) == 0)
                               ? 0ul
                               : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_NET_SET_LOCAL_IP:
        {
            unsigned char ip[4];
            if (process_copy_from_user(ip, (const void*)frame[TF_X0], sizeof(ip)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            net_proto_set_local_ip(ip);
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_NET_GET_GATEWAY_IP:
        {
            unsigned char ip[4];
            net_proto_get_gateway_ip(ip);
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0], ip, sizeof(ip)) == 0)
                               ? 0ul
                               : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_NET_SET_GATEWAY_IP:
        {
            unsigned char ip[4];
            if (process_copy_from_user(ip, (const void*)frame[TF_X0], sizeof(ip)) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            net_proto_set_gateway_ip(ip);
            frame[TF_X0] = 0;
            return frame_sp;
        }

        case SYS_AUTH_IS_READY:
            frame[TF_X0] = (unsigned long)auth_is_ready();
            return frame_sp;

        case SYS_AUTH_GET_USERNAME:
        {
            char kname[AUTH_USERNAME_MAX + 1u];
            unsigned int user_cap = (unsigned int)frame[TF_X1];
            unsigned int copy_len = 0u;

            if (!auth_is_ready()){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (user_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            for (unsigned int i = 0; i < sizeof(kname); i++){
                kname[i] = 0;
            }
            if (copy_cstr_out(kname, (unsigned int)sizeof(kname), auth_username()) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            copy_len = cstr_bytes_with_nul(kname, (unsigned int)sizeof(kname));
            if (copy_len == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if (user_cap < copy_len){
                copy_len = user_cap;
                kname[copy_len - 1u] = 0;
            }
            frame[TF_X0] = (process_copy_to_user((void*)frame[TF_X0], kname, copy_len) == 0)
                               ? 0ul
                               : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_AUTH_VERIFY_PASSWORD:
        {
            char username[AUTH_USERNAME_MAX + 1u];
            char password[USER_PASS_MAX];
            int auth_rc = -1;
            if (copy_cstr_from_user_bound(username, sizeof(username), (const char*)frame[TF_X0]) != 0 ||
                copy_cstr_from_user_bound(password, sizeof(password), (const char*)frame[TF_X1]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            auth_rc = auth_verify_password(username, password);
            for (unsigned int i = 0; i < sizeof(password); i++){
                password[i] = 0;
            }
            for (unsigned int i = 0; i < sizeof(username); i++){
                username[i] = 0;
            }
            frame[TF_X0] = (unsigned long)auth_rc;
            return frame_sp;
        }

        case SYS_WIFI_INIT:
            frame[TF_X0] = (unsigned long)cyw43_init();
            return frame_sp;

        case SYS_WIFI_LOAD_FW:
        {
            char fw_name[32];
            char nv_name[32];
            const char* fw_arg = 0;
            const char* nv_arg = 0;

            if ((const void*)frame[TF_X0]){
                if (copy_cstr_from_user_bound(fw_name, sizeof(fw_name), (const char*)frame[TF_X0]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                fw_arg = fw_name;
            }
            if ((const void*)frame[TF_X1]){
                if (copy_cstr_from_user_bound(nv_name, sizeof(nv_name), (const char*)frame[TF_X1]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                nv_arg = nv_name;
            }
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)cyw43_upload_firmware_from_fat(fw_arg, nv_arg);
            kernel_preempt_exit();
            return frame_sp;
        }

        case SYS_WIFI_UP:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_up();
            return frame_sp;

        case SYS_WIFI_DOWN:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_down();
            return frame_sp;

        case SYS_WIFI_SCAN: {
            cyw43_scan_result_t* user_out = (cyw43_scan_result_t*)frame[TF_X0];
            unsigned int cap = clamp_u32((unsigned int)frame[TF_X1], USER_WIFI_SCAN_MAX);
            cyw43_scan_result_t* kout = 0;
            unsigned int count = 0;
            int rc = -1;

            if (!user_out || cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (cyw43_scan_result_t*)kmalloc((unsigned long)sizeof(cyw43_scan_result_t) * cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            rc = cyw43_ioctl_scan(kout, cap, &count);
            if (rc == 0){
                if (count > cap){
                    count = cap;
                }
                if (count > 0u &&
                    process_copy_to_user(user_out,
                                         kout,
                                         (unsigned long)sizeof(cyw43_scan_result_t) * count) != 0){
                    rc = -1;
                }
            }
            kfree_secure(kout, (unsigned long)sizeof(cyw43_scan_result_t) * cap);
            frame[TF_X0] = (rc == 0) ? (unsigned long)count : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_WIFI_JOIN:
        {
            char ssid[33];
            char password[USER_PASS_MAX];
            const char* pass_arg = 0;
            int join_rc = -1;
            if (copy_cstr_from_user_bound(ssid, sizeof(ssid), (const char*)frame[TF_X0]) != 0){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            if ((const void*)frame[TF_X1]){
                if (copy_cstr_from_user_bound(password, sizeof(password), (const char*)frame[TF_X1]) != 0){
                    frame[TF_X0] = (unsigned long)-1;
                    return frame_sp;
                }
                pass_arg = password;
            }
            join_rc = cyw43_ioctl_join(ssid, pass_arg);
            for (unsigned int i = 0; i < sizeof(password); i++){
                password[i] = 0;
            }
            for (unsigned int i = 0; i < sizeof(ssid); i++){
                ssid[i] = 0;
            }
            frame[TF_X0] = (unsigned long)join_rc;
            return frame_sp;
        }

        case SYS_WIFI_DUMP_STATUS:
            cyw43_dump_status();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_WIFI_GET_VERSION:
        {
            char* user_out = (char*)frame[TF_X0];
            unsigned int user_cap = clamp_u32((unsigned int)frame[TF_X1], USER_CSTR_MAX);
            char* kout = 0;
            int rc = -1;

            if (!user_out || user_cap == 0u){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kout = (char*)kmalloc(user_cap);
            if (!kout){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            rc = cyw43_get_firmware_version(kout, user_cap);
            if (rc == 0){
                unsigned int copy_len = cstr_bytes_with_nul(kout, user_cap);
                if (copy_len == 0u){
                    copy_len = 1u;
                }
                if (process_copy_to_user(user_out, kout, copy_len) != 0){
                    rc = -1;
                }
            }
            kfree_secure(kout, user_cap);
            frame[TF_X0] = (unsigned long)rc;
            return frame_sp;
        }

        default:
            frame[TF_X0] = (unsigned long)-1;
            return frame_sp;
    }
}
