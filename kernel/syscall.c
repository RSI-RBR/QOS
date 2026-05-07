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

#define ESR_EC_SHIFT 26
#define ESR_EC_MASK   0x3FUL
#define ESR_EC_SVC64  0x15UL

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

void* syscall_handle(void* frame_sp, unsigned long esr){
    unsigned long ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    unsigned long* frame = (unsigned long*)frame_sp;

    if (ec != ESR_EC_SVC64 || !frame){
        return frame_sp;
    }

    unsigned long nr = frame[TF_X8];

    switch (nr){
        case SYS_PUTC:
            if (console_get_owner() == process_current_pid()){
                remote_login_on_tty_output_char((char)frame[TF_X0]);
            }
            uart_send((char)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_PUTS:
            syscall_write_puts(process_current_pid(), (const char*)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_SLEEP: {
            unsigned int ms = (unsigned int)frame[TF_X0];
            process_sleep(ms);
            frame[TF_X0] = 0;
            return frame_sp;
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
            (void)net_poll();
            remote_login_poll();
            if (console_try_getc_for_pid(process_current_pid(), &c)){
                frame[TF_X0] = (unsigned long)(unsigned char)c;
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
            if (!fat_name_83){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            kernel_preempt_enter();
            loaded_program_t prog = load_program_from_sd_named(fat_name_83);
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
            frame[TF_X0] = (unsigned long)net_recv_raw((unsigned char*)frame[TF_X0],
                                                       (unsigned int)frame[TF_X1]);
            return frame_sp;

        case SYS_NET_SEND_RAW:
            frame[TF_X0] = (unsigned long)net_send_raw((const unsigned char*)frame[TF_X0],
                                                       (unsigned int)frame[TF_X1]);
            return frame_sp;

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
            frame[TF_X0] = (unsigned long)udp_recv_next((unsigned char*)frame[TF_X1],
                                                        (unsigned int)frame[TF_X2],
                                                        (udp_meta_t*)frame[TF_X0]);
            return frame_sp;

        case SYS_NET_UDP_SEND:
            frame[TF_X0] = (unsigned long)udp_send((const unsigned char*)frame[TF_X0],
                                                   (unsigned short)frame[TF_X1],
                                                   (unsigned short)frame[TF_X2],
                                                   (const unsigned char*)frame[TF_X3],
                                                   (unsigned int)frame[TF_X4]);
            return frame_sp;

        case SYS_NET_TCP_HTTP_GET:
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)tcp_http_get((const unsigned char*)frame[TF_X0],
                                                       (const char*)frame[TF_X1],
                                                       (const char*)frame[TF_X2],
                                                       (unsigned char*)frame[TF_X3],
                                                       (unsigned int)frame[TF_X4]);
            kernel_preempt_exit();
            return frame_sp;

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
            frame[TF_X0] = (unsigned long)ksocket_connect(pid,
                                                          (int)frame[TF_X0],
                                                          (const qos_sockaddr_in_t*)frame[TF_X1],
                                                          (unsigned int)frame[TF_X2]);
            return frame_sp;
        }

        case SYS_SOCKET_SEND: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ksocket_send(pid,
                                                       (int)frame[TF_X0],
                                                       (const unsigned char*)frame[TF_X1],
                                                       (unsigned int)frame[TF_X2],
                                                       (unsigned int)frame[TF_X3]);
            return frame_sp;
        }

        case SYS_SOCKET_RECV: {
            int pid = process_current_pid();
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)ksocket_recv(pid,
                                                       (int)frame[TF_X0],
                                                       (unsigned char*)frame[TF_X1],
                                                       (unsigned int)frame[TF_X2],
                                                       (unsigned int)frame[TF_X3]);
            kernel_preempt_exit();
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
            frame[TF_X0] = (unsigned long)ktls_get_local_public(pid,
                                                                 (int)frame[TF_X0],
                                                                 (unsigned char*)frame[TF_X1]);
            return frame_sp;
        }

        case SYS_TLS_SET_PEER_PUBLIC: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_set_peer_public(pid,
                                                                (int)frame[TF_X0],
                                                                (const unsigned char*)frame[TF_X1]);
            return frame_sp;
        }

        case SYS_TLS_BUILD_CLIENT_HELLO: {
            int pid = process_current_pid();
            unsigned int out_len = 0;
            int rc = ktls_build_client_hello(pid,
                                             (int)frame[TF_X0],
                                             (unsigned char*)frame[TF_X1],
                                             (unsigned int)frame[TF_X2],
                                             &out_len);
            frame[TF_X0] = (rc == 0) ? (unsigned long)out_len : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_TLS_PROCESS_SERVER_HELLO: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_process_server_hello(pid,
                                                                     (int)frame[TF_X0],
                                                                     (const unsigned char*)frame[TF_X1],
                                                                     (unsigned int)frame[TF_X2]);
            return frame_sp;
        }

        case SYS_TLS_PROCESS_CLIENT_HELLO_BUILD_SERVER_HELLO: {
            int pid = process_current_pid();
            unsigned int out_len = 0;
            int rc = ktls_process_client_hello_build_server_hello(pid,
                                                                   (int)frame[TF_X0],
                                                                   (const unsigned char*)frame[TF_X1],
                                                                   (unsigned int)frame[TF_X2],
                                                                   (unsigned char*)frame[TF_X3],
                                                                   (unsigned int)frame[TF_X4],
                                                                   &out_len);
            frame[TF_X0] = (rc == 0) ? (unsigned long)out_len : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_TLS_RECORD_ENCRYPT: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_record_encrypt(pid,
                                                               (int)frame[TF_X0],
                                                               (qos_tls_record_io_t*)frame[TF_X1]);
            return frame_sp;
        }

        case SYS_TLS_RECORD_DECRYPT: {
            int pid = process_current_pid();
            frame[TF_X0] = (unsigned long)ktls_record_decrypt(pid,
                                                               (int)frame[TF_X0],
                                                               (qos_tls_record_io_t*)frame[TF_X1]);
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

        case SYS_NET_GET_LOCAL_IP:
            net_proto_get_local_ip((unsigned char*)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_NET_SET_LOCAL_IP:
            net_proto_set_local_ip((const unsigned char*)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_WIFI_INIT:
            frame[TF_X0] = (unsigned long)cyw43_init();
            return frame_sp;

        case SYS_WIFI_LOAD_FW:
            kernel_preempt_enter();
            frame[TF_X0] = (unsigned long)cyw43_upload_firmware_from_fat((const char*)frame[TF_X0],
                                                                          (const char*)frame[TF_X1]);
            kernel_preempt_exit();
            return frame_sp;

        case SYS_WIFI_UP:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_up();
            return frame_sp;

        case SYS_WIFI_DOWN:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_down();
            return frame_sp;

        case SYS_WIFI_SCAN: {
            unsigned int count = 0;
            int rc = cyw43_ioctl_scan((cyw43_scan_result_t*)frame[TF_X0],
                                      (unsigned int)frame[TF_X1],
                                      &count);
            frame[TF_X0] = (rc == 0) ? (unsigned long)count : (unsigned long)-1;
            return frame_sp;
        }

        case SYS_WIFI_JOIN:
            frame[TF_X0] = (unsigned long)cyw43_ioctl_join((const char*)frame[TF_X0],
                                                           (const char*)frame[TF_X1]);
            return frame_sp;

        case SYS_WIFI_DUMP_STATUS:
            cyw43_dump_status();
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_WIFI_GET_VERSION:
            frame[TF_X0] = (unsigned long)cyw43_get_firmware_version((char*)frame[TF_X0],
                                                                      (unsigned int)frame[TF_X1]);
            return frame_sp;

        default:
            frame[TF_X0] = (unsigned long)-1;
            return frame_sp;
    }
}
