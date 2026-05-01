#include "syscall.h"
#include "uart.h"
#include "process.h"
#include "framebuffer.h"
#include "timer.h"

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
        uart_send(s[i]);
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

        default:
            frame[TF_X0] = (unsigned long)-1;
            return frame_sp;
    }
}
