#include "syscall.h"
#include "uart.h"
#include "process.h"
#include "framebuffer.h"
#include "timer.h"
#include "loader.h"
#include "memory.h"
#include "net.h"
#include "usb_host.h"

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

static void syscall_write_puts(const char* s){
    if (!s){
        return;
    }
    unsigned long n = clamp_puts_len(s);
    for (unsigned long i = 0; i < n; i++){
        if (s[i] == '\n'){
            uart_send('\r');
        }
        uart_send(s[i]);
    }
}

static void syscall_dump_usb_info(void){
    usb_root_device_info_t info;
    if (usb_host_get_root_device_info(&info) != 0){
        syscall_write_puts("USB root: not enumerated\n");
        return;
    }

    syscall_write_puts("USB root addr=");
    uart_puthex(info.address);
    syscall_write_puts(" vid=");
    uart_puthex(info.vid);
    syscall_write_puts(" pid=");
    uart_puthex(info.pid);
    syscall_write_puts(" class=");
    uart_puthex(info.dev_class);
    syscall_write_puts(" cfg=");
    uart_puthex(info.config_value);
    syscall_write_puts(info.configured ? " (set)\n" : " (not set)\n");

    if (info.child_present){
        syscall_write_puts("USB child addr=");
        uart_puthex(info.child_address);
        syscall_write_puts(" vid=");
        uart_puthex(info.child_vid);
        syscall_write_puts(" pid=");
        uart_puthex(info.child_pid);
        syscall_write_puts(" class=");
        uart_puthex(info.child_class);
        syscall_write_puts(" cfg=");
        uart_puthex(info.child_config_value);
        syscall_write_puts(info.child_configured ? " (set)\n" : " (not set)\n");
        syscall_write_puts("USB child bulk in=");
        uart_puthex(info.child_bulk_in_ep);
        syscall_write_puts(" mps=");
        uart_puthex(info.child_bulk_in_mps);
        syscall_write_puts(" out=");
        uart_puthex(info.child_bulk_out_ep);
        syscall_write_puts(" mps=");
        uart_puthex(info.child_bulk_out_mps);
        syscall_write_puts("\n");
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
            uart_send((char)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_PUTS:
            syscall_write_puts((const char*)frame[TF_X0]);
            frame[TF_X0] = 0;
            return frame_sp;

        case SYS_SLEEP: {
            unsigned int ms = (unsigned int)frame[TF_X0];
            process_t* cur = get_current_process();
            if (!cur){
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }
            cur->wake_tick = system_ticks + ms;
            cur->state = PROC_SLEEPING;
            frame[TF_X0] = 0;
            return scheduler_on_irq(frame_sp);
        }

        case SYS_EXIT:
            process_fault_current();
            frame[TF_X0] = 0;
            return scheduler_on_irq(frame_sp);

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
            if (uart_try_getc(&c)){
                frame[TF_X0] = (unsigned long)(unsigned char)c;
            } else{
                frame[TF_X0] = (unsigned long)-1;
            }
            return frame_sp;
        }

        case SYS_RUN_PROGRAM: {
            loaded_program_t prog = load_program_from_sd();
            if (!prog.entry){
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
                frame[TF_X0] = (unsigned long)-1;
                return frame_sp;
            }

            frame[TF_X0] = (unsigned long)pid;
            return frame_sp;
        }

        case SYS_NET_DUMP_STATS:
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

        default:
            frame[TF_X0] = (unsigned long)-1;
            return frame_sp;
    }
}
