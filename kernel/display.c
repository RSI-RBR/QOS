#include "display.h"
#include "dma.h"
#include "framebuffer.h"
#include "klog.h"
#include "memory.h"
#include "program.h"
#include "spinlock.h"
#include "timer.h"
#include "usb_host.h"

static display_session_t g_display_sessions[DISPLAY_MAX_SESSIONS];
static spinlock_t g_display_lock;
static int g_display_ready = 0;
static int g_display_active = DISPLAY_TEXT_SESSION_ID;
static int g_display_pending_switch_pid = -1;

#define DISPLAY_CURSOR_RADIUS 7
#define DISPLAY_CURSOR_PAD 2
#define DISPLAY_CURSOR_MAX_SIDE ((DISPLAY_CURSOR_RADIUS + DISPLAY_CURSOR_PAD) * 2 + 1)
#define DISPLAY_SCANOUT_PAGE 1u
#define DISPLAY_DMA_MIN_BYTES (256u * 1024u)
#define DISPLAY_DMA_FULL_FRAME_THRESHOLD_NUM 1u
#define DISPLAY_DMA_FULL_FRAME_THRESHOLD_DEN 2u
#define DISPLAY_FRAMEBUFFER_ALIGN 64UL
#define DISPLAY_HOTKEY_POLL_MS 16UL

static int g_display_gpu_enabled = 0;
static unsigned int g_display_gpu_flip_count = 0;
static unsigned int g_display_gpu_failure_count = 0;
static unsigned long g_display_next_hotkey_poll_tick = 0UL;
static int g_cursor_drawn = 0;
static int g_cursor_session_id = -1;
static int g_cursor_x = 0;
static int g_cursor_y = 0;
static unsigned int g_cursor_seq = 0u;
static int g_cursor_saved_valid = 0;
static int g_cursor_saved_session_id = -1;
static unsigned int g_cursor_saved_x0 = 0u;
static unsigned int g_cursor_saved_y0 = 0u;
static unsigned int g_cursor_saved_x1 = 0u;
static unsigned int g_cursor_saved_y1 = 0u;
static unsigned int g_cursor_saved_pixels[DISPLAY_CURSOR_MAX_SIDE * DISPLAY_CURSOR_MAX_SIDE];

typedef struct {
    unsigned long present_calls;
    unsigned long pageflip_calls;
    unsigned long buffered_calls;
    unsigned long cursor_only_calls;
    unsigned long no_work_calls;
    unsigned long full_dirty_calls;
    unsigned long partial_dirty_calls;
    unsigned long copied_pixels;
    unsigned long present_us;
    unsigned long present_max_us;
    unsigned long usb_poll_us;
    unsigned long attach_us;
    unsigned long copy_us;
    unsigned long cursor_us;
    unsigned long flip_us;
} display_profile_t;

static display_profile_t g_display_profile;

static unsigned long display_read_cntpct(void){
    unsigned long v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static unsigned long display_read_cntfrq(void){
    unsigned long v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static unsigned long display_cycles_to_us(unsigned long cycles, unsigned long hz){
    if (hz == 0UL){
        return 0UL;
    }
    return (unsigned long)(((unsigned long long)cycles * 1000000ULL) /
                           (unsigned long long)hz);
}

static unsigned long display_elapsed_us(unsigned long start, unsigned long hz){
    return display_cycles_to_us(display_read_cntpct() - start, hz);
}

static int display_valid_id(int session_id){
    return session_id >= 0 && session_id < DISPLAY_MAX_SESSIONS;
}

static int display_find_graphics_for_pid_locked(int owner_pid){
    if (owner_pid < 0){
        return -1;
    }
    for (int i = 1; i < DISPLAY_MAX_SESSIONS; i++){
        if (g_display_sessions[i].type == DISPLAY_GRAPHICS &&
            g_display_sessions[i].owner_pid == owner_pid){
            return i;
        }
    }
    return -1;
}

static unsigned long display_fb_size(void){
    unsigned int width = fb_get_width();
    unsigned int height = fb_get_height();
    unsigned int pitch = fb_get_pitch();

    if (pitch == 0u){
        pitch = width * sizeof(unsigned int);
    }
    if (height == 0u || pitch == 0u){
        return 0UL;
    }
    if ((~0UL / (unsigned long)height) < (unsigned long)pitch){
        return 0UL;
    }
    return (unsigned long)pitch * (unsigned long)height;
}

static void display_memset(void* ptr, unsigned char value, unsigned long len){
    unsigned char* p = (unsigned char*)ptr;
    if (!p){
        return;
    }
    for (unsigned long i = 0; i < len; i++){
        p[i] = value;
    }
}

static unsigned long display_align_up(unsigned long v, unsigned long align){
    return (v + (align - 1UL)) & ~(align - 1UL);
}

static void display_fill_u32(unsigned int* dst, unsigned int color, unsigned long count){
    if (!dst){
        return;
    }
    while (count >= 8ul){
        dst[0] = color;
        dst[1] = color;
        dst[2] = color;
        dst[3] = color;
        dst[4] = color;
        dst[5] = color;
        dst[6] = color;
        dst[7] = color;
        dst += 8;
        count -= 8ul;
    }
    while (count--){
        *dst++ = color;
    }
}

static void display_copy_u32(unsigned int* dst, const unsigned int* src, unsigned long count){
    if (!dst || !src){
        return;
    }
    while (count >= 8ul){
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];
        dst += 8;
        src += 8;
        count -= 8ul;
    }
    while (count--){
        *dst++ = *src++;
    }
}

static void display_copy_surface(void* dst,
                                 unsigned int dst_pitch,
                                 const void* src,
                                 unsigned int src_pitch,
                                 unsigned int width,
                                 unsigned int height){
    if (!dst || !src || dst_pitch < width * sizeof(unsigned int) ||
        src_pitch < width * sizeof(unsigned int)){
        return;
    }
    for (unsigned int y = 0; y < height; y++){
        unsigned int* d = (unsigned int*)((unsigned char*)dst +
                                          ((unsigned long)y * dst_pitch));
        const unsigned int* s = (const unsigned int*)((const unsigned char*)src +
                                                      ((unsigned long)y * src_pitch));
        display_copy_u32(d, s, width);
    }
}

static int display_pageflip_available(void){
    unsigned long page_bytes = display_fb_size();
    unsigned long pool_start = QOS_PROGRAM_POOL_START;
    unsigned long pool_end = QOS_PROGRAM_POOL_START + QOS_PROGRAM_POOL_SIZE;
    unsigned int pages = fb_get_page_count();

    if (pages <= DISPLAY_SCANOUT_PAGE || page_bytes == 0UL){
        return 0;
    }

    for (unsigned int page = 0u; page < pages; page++){
        unsigned long base = fb_get_page_base(page);
        if (base == 0UL){
            return 0;
        }
        if (base < pool_end && base + page_bytes > pool_start){
            return 0;
        }
    }
    return 1;
}

static unsigned int display_choose_scanout_page(unsigned int avoid_a,
                                                unsigned int avoid_b,
                                                int require_two_avoids){
    unsigned int pages = fb_get_page_count();
    unsigned int first_graphics_page = (pages >= 4u) ? 1u : 0u;
    for (unsigned int page = first_graphics_page; page < pages; page++){
        if (page == avoid_a || page == avoid_b){
            continue;
        }
        if (fb_get_page_base(page) != 0UL){
            return page;
        }
    }
    if (require_two_avoids){
        return pages;
    }
    for (unsigned int page = first_graphics_page; page < pages; page++){
        if (page == avoid_a){
            continue;
        }
        if (fb_get_page_base(page) != 0UL){
            return page;
        }
    }
    for (unsigned int page = 0u; page < pages; page++){
        if (page == avoid_a || (require_two_avoids && page == avoid_b)){
            continue;
        }
        if (fb_get_page_base(page) != 0UL){
            return page;
        }
    }
    return pages;
}

static void display_invalidate_cursor_locked(void){
    g_cursor_drawn = 0;
    g_cursor_session_id = -1;
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_cursor_seq = 0u;
    g_cursor_saved_valid = 0;
    g_cursor_saved_session_id = -1;
}

static void display_restore_cursor_saved_locked(unsigned long dst_base,
                                                unsigned int dst_pitch,
                                                unsigned int dst_width,
                                                unsigned int dst_height,
                                                int session_id){
    if (!g_cursor_saved_valid ||
        g_cursor_saved_session_id != session_id ||
        !dst_base || dst_pitch == 0u){
        return;
    }
    if (g_cursor_saved_x1 > dst_width || g_cursor_saved_y1 > dst_height ||
        g_cursor_saved_x0 >= g_cursor_saved_x1 ||
        g_cursor_saved_y0 >= g_cursor_saved_y1){
        g_cursor_saved_valid = 0;
        return;
    }

    unsigned int w = g_cursor_saved_x1 - g_cursor_saved_x0;
    unsigned int h = g_cursor_saved_y1 - g_cursor_saved_y0;
    if (w > DISPLAY_CURSOR_MAX_SIDE || h > DISPLAY_CURSOR_MAX_SIDE){
        g_cursor_saved_valid = 0;
        return;
    }

    for (unsigned int y = 0; y < h; y++){
        unsigned int* dst = (unsigned int*)((unsigned char*)dst_base +
                                            ((unsigned long)(g_cursor_saved_y0 + y) * dst_pitch));
        dst += g_cursor_saved_x0;
        const unsigned int* src = &g_cursor_saved_pixels[y * DISPLAY_CURSOR_MAX_SIDE];
        display_copy_u32(dst, src, w);
    }
    g_cursor_saved_valid = 0;
}

static void display_save_cursor_under_locked(unsigned long dst_base,
                                             unsigned int dst_pitch,
                                             unsigned int dst_width,
                                             unsigned int dst_height,
                                             int session_id,
                                             unsigned int x0,
                                             unsigned int y0,
                                             unsigned int x1,
                                             unsigned int y1){
    g_cursor_saved_valid = 0;
    if (!dst_base || dst_pitch == 0u || x0 >= x1 || y0 >= y1 ||
        x1 > dst_width || y1 > dst_height){
        return;
    }

    unsigned int w = x1 - x0;
    unsigned int h = y1 - y0;
    if (w > DISPLAY_CURSOR_MAX_SIDE || h > DISPLAY_CURSOR_MAX_SIDE){
        return;
    }

    for (unsigned int y = 0; y < h; y++){
        const unsigned int* src = (const unsigned int*)((const unsigned char*)dst_base +
                                                        ((unsigned long)(y0 + y) * dst_pitch));
        src += x0;
        unsigned int* dst = &g_cursor_saved_pixels[y * DISPLAY_CURSOR_MAX_SIDE];
        display_copy_u32(dst, src, w);
    }
    g_cursor_saved_x0 = x0;
    g_cursor_saved_y0 = y0;
    g_cursor_saved_x1 = x1;
    g_cursor_saved_y1 = y1;
    g_cursor_saved_session_id = session_id;
    g_cursor_saved_valid = 1;
}

static int display_cursor_rect(int x,
                               int y,
                               unsigned int max_w,
                               unsigned int max_h,
                               unsigned int* out_x0,
                               unsigned int* out_y0,
                               unsigned int* out_x1,
                               unsigned int* out_y1){
    if (!out_x0 || !out_y0 || !out_x1 || !out_y1 || max_w == 0u || max_h == 0u){
        return -1;
    }

    int left = x - DISPLAY_CURSOR_RADIUS - DISPLAY_CURSOR_PAD;
    int top = y - DISPLAY_CURSOR_RADIUS - DISPLAY_CURSOR_PAD;
    int right = x + DISPLAY_CURSOR_RADIUS + DISPLAY_CURSOR_PAD + 1;
    int bottom = y + DISPLAY_CURSOR_RADIUS + DISPLAY_CURSOR_PAD + 1;

    if (left < 0){
        left = 0;
    }
    if (top < 0){
        top = 0;
    }
    if (right > (int)max_w){
        right = (int)max_w;
    }
    if (bottom > (int)max_h){
        bottom = (int)max_h;
    }
    if (left >= right || top >= bottom){
        return -1;
    }

    *out_x0 = (unsigned int)left;
    *out_y0 = (unsigned int)top;
    *out_x1 = (unsigned int)right;
    *out_y1 = (unsigned int)bottom;
    return 0;
}

static int display_rect_contains(unsigned int outer_x0,
                                 unsigned int outer_y0,
                                 unsigned int outer_x1,
                                 unsigned int outer_y1,
                                 unsigned int inner_x0,
                                 unsigned int inner_y0,
                                 unsigned int inner_x1,
                                 unsigned int inner_y1){
    return outer_x0 <= inner_x0 &&
           outer_y0 <= inner_y0 &&
           outer_x1 >= inner_x1 &&
           outer_y1 >= inner_y1;
}

static int display_copy_rect_locked(const display_session_t* s,
                                    unsigned long dst_base,
                                    unsigned int dst_pitch,
                                    unsigned int dst_width,
                                    unsigned int dst_height,
                                    unsigned int x0,
                                    unsigned int y0,
                                    unsigned int x1,
                                    unsigned int y1){
    if (!s || !s->framebuffer || !dst_base || s->pitch == 0u || dst_pitch == 0u){
        return -1;
    }
    if (x0 >= x1 || y0 >= y1){
        return -1;
    }
    if (x0 >= s->width || y0 >= s->height || x0 >= dst_width || y0 >= dst_height){
        return -1;
    }
    if (x1 > s->width){
        x1 = s->width;
    }
    if (y1 > s->height){
        y1 = s->height;
    }
    if (x1 > dst_width){
        x1 = dst_width;
    }
    if (y1 > dst_height){
        y1 = dst_height;
    }
    if (x0 >= x1 || y0 >= y1){
        return -1;
    }

    unsigned int copy_width = x1 - x0;
    unsigned int copy_height = y1 - y0;
    unsigned int row_bytes = copy_width * sizeof(unsigned int);
    unsigned int row_offset = x0 * sizeof(unsigned int);
    unsigned long copy_bytes = (unsigned long)row_bytes * (unsigned long)copy_height;
    unsigned long fb_bus = fb_get_bus_base();
    if (dst_pitch < row_offset || s->pitch < row_offset ||
        dst_pitch - row_offset < row_bytes ||
        s->pitch - row_offset < row_bytes){
        return -1;
    }

    int full_frame_copy = (x0 == 0u &&
                           y0 == 0u &&
                           x1 == s->width &&
                           y1 == s->height &&
                           x1 <= dst_width &&
                           y1 <= dst_height);

    if (full_frame_copy &&
        dma_is_enabled() &&
        fb_bus != 0u &&
        copy_bytes >= DISPLAY_DMA_MIN_BYTES &&
        copy_bytes <= 0xFFFFFFFFUL){
        unsigned int src_stride = s->pitch - row_bytes;
        unsigned int dst_stride = dst_pitch - row_bytes;
        const void* src0 = (const void*)((const unsigned char*)s->framebuffer +
                                         ((unsigned long)y0 * s->pitch) +
                                         row_offset);
        unsigned int dst_bus = (unsigned int)(fb_bus +
                                              ((unsigned long)y0 * dst_pitch) +
                                              row_offset);
        if (src_stride == 0u && dst_stride == 0u){
            if (dma_memcpy_to_bus(dst_bus, src0, (unsigned int)copy_bytes) == 0){
                return 0;
            }
        } else if (src_stride <= 0x7FFFu && dst_stride <= 0x7FFFu){
            /*
             * Still one DMA control block for the whole visible frame, even
             * when firmware pads either pitch. This avoids the old trap of
             * treating graphics present as many tiny row transfers.
             */
            if (dma_memcpy_2d_to_bus(dst_bus,
                                     dst_stride,
                                     src0,
                                     src_stride,
                                     row_bytes,
                                     copy_height) == 0){
                return 0;
            }
        }
    }

    for (unsigned int y = 0; y < copy_height; y++){
        unsigned int* src = (unsigned int*)((unsigned char*)s->framebuffer +
                                            ((unsigned long)(y0 + y) * s->pitch) +
                                            row_offset);
        unsigned int* dst = (unsigned int*)((unsigned char*)dst_base +
                                            ((unsigned long)(y0 + y) * dst_pitch) +
                                            row_offset);
        display_copy_u32(dst, src, copy_width);
    }
    return 0;
}

static void display_draw_cursor_overlay(unsigned long dst_base,
                                        unsigned int dst_pitch,
                                        unsigned int dst_width,
                                        unsigned int dst_height,
                                        int x,
                                        int y,
                                        unsigned int buttons){
    if (!dst_base || dst_pitch == 0u || dst_width == 0u || dst_height == 0u){
        return;
    }

    unsigned int color = (buttons & 0x1u) ? 0x00FF7040u : 0x00FFFFFFu;
    unsigned int outline = 0x00000000u;

    for (int d = -DISPLAY_CURSOR_RADIUS; d <= DISPLAY_CURSOR_RADIUS; d++){
        int px = x + d;
        int py = y + d;
        if (py >= 0 && py < (int)dst_height){
            if (x >= 0 && x < (int)dst_width){
                unsigned int* row = (unsigned int*)((unsigned char*)dst_base +
                                                    ((unsigned long)py * dst_pitch));
                row[x] = color;
                if (x > 0){
                    row[x - 1] = outline;
                }
                if (x + 1 < (int)dst_width){
                    row[x + 1] = outline;
                }
            }
        }
        if (y >= 0 && y < (int)dst_height){
            if (px >= 0 && px < (int)dst_width){
                unsigned int* row = (unsigned int*)((unsigned char*)dst_base +
                                                    ((unsigned long)y * dst_pitch));
                row[px] = color;
                if (y > 0){
                    unsigned int* r0 = (unsigned int*)((unsigned char*)dst_base +
                                                       ((unsigned long)(y - 1) * dst_pitch));
                    r0[px] = outline;
                }
                if (y + 1 < (int)dst_height){
                    unsigned int* r1 = (unsigned int*)((unsigned char*)dst_base +
                                                       ((unsigned long)(y + 1) * dst_pitch));
                    r1[px] = outline;
                }
            }
        }
    }

    if (x >= 0 && x < (int)dst_width && y >= 0 && y < (int)dst_height){
        unsigned int* row = (unsigned int*)((unsigned char*)dst_base +
                                            ((unsigned long)y * dst_pitch));
        row[x] = 0x0000FF00u;
    }
}

static void display_clear_session_locked(int session_id){
    if (!display_valid_id(session_id)){
        return;
    }
    g_display_sessions[session_id].session_id = session_id;
    g_display_sessions[session_id].owner_pid = -1;
    g_display_sessions[session_id].type = DISPLAY_NONE;
    g_display_sessions[session_id].active = 0;
    g_display_sessions[session_id].framebuffer = 0;
    g_display_sessions[session_id].framebuffer_size = 0UL;
    g_display_sessions[session_id].backing_framebuffer = 0;
    g_display_sessions[session_id].backing_framebuffer_size = 0UL;
    g_display_sessions[session_id].allocation = 0;
    g_display_sessions[session_id].allocation_size = 0UL;
    g_display_sessions[session_id].dirty = 0u;
    g_display_sessions[session_id].dirty_x0 = 0u;
    g_display_sessions[session_id].dirty_y0 = 0u;
    g_display_sessions[session_id].dirty_x1 = 0u;
    g_display_sessions[session_id].dirty_y1 = 0u;
    g_display_sessions[session_id].width = 0u;
    g_display_sessions[session_id].height = 0u;
    g_display_sessions[session_id].pitch = 0u;
    g_display_sessions[session_id].scanout_attached = 0;
    g_display_sessions[session_id].scanout_page = 0u;
}

static void display_mark_full_dirty_locked(display_session_t* s){
    if (!s || s->width == 0u || s->height == 0u){
        return;
    }
    s->dirty = 1u;
    s->dirty_x0 = 0u;
    s->dirty_y0 = 0u;
    s->dirty_x1 = s->width;
    s->dirty_y1 = s->height;
}

static void display_mark_rect_dirty_locked(display_session_t* s,
                                           unsigned int x,
                                           unsigned int y,
                                           unsigned int w,
                                           unsigned int h){
    if (!s || w == 0u || h == 0u || x >= s->width || y >= s->height){
        return;
    }
    if (x + w < x || x + w > s->width){
        w = s->width - x;
    }
    if (y + h < y || y + h > s->height){
        h = s->height - y;
    }

    unsigned int x1 = x + w;
    unsigned int y1 = y + h;
    if (!s->dirty){
        s->dirty = 1u;
        s->dirty_x0 = x;
        s->dirty_y0 = y;
        s->dirty_x1 = x1;
        s->dirty_y1 = y1;
        return;
    }
    if (x < s->dirty_x0){
        s->dirty_x0 = x;
    }
    if (y < s->dirty_y0){
        s->dirty_y0 = y;
    }
    if (x1 > s->dirty_x1){
        s->dirty_x1 = x1;
    }
    if (y1 > s->dirty_y1){
        s->dirty_y1 = y1;
    }
}

static void display_detach_scanout_locked(display_session_t* s){
    if (!s || !s->scanout_attached){
        return;
    }

    unsigned int visible_page = fb_get_display_page();
    void* scanout = 0;
    if (g_display_active == s->session_id && visible_page < fb_get_page_count()){
        scanout = (void*)fb_get_page_base(visible_page);
    }
    if (!scanout){
        scanout = s->framebuffer;
    }
    if (scanout && s->backing_framebuffer &&
        s->backing_framebuffer_size >= display_fb_size()){
        display_copy_surface(s->backing_framebuffer,
                             s->pitch,
                             scanout,
                             s->pitch,
                             s->width,
                             s->height);
    }

    s->framebuffer = s->backing_framebuffer;
    s->framebuffer_size = s->backing_framebuffer_size;
    s->scanout_attached = 0;
    s->scanout_page = 0u;
    display_invalidate_cursor_locked();
}

static int display_attach_scanout_locked(display_session_t* s){
    if (!g_display_gpu_enabled || !display_pageflip_available()){
        return -1;
    }
    if (!s || s->type != DISPLAY_GRAPHICS || !s->backing_framebuffer ||
        s->width == 0u || s->height == 0u || s->pitch == 0u){
        return -1;
    }
    if (s->scanout_attached){
        return 0;
    }

    unsigned int visible_page = fb_get_display_page();
    unsigned int scanout_page = display_choose_scanout_page(visible_page,
                                                            visible_page,
                                                            0);
    if (scanout_page >= fb_get_page_count()){
        g_display_gpu_failure_count++;
        return -1;
    }

    void* scanout = (void*)fb_get_page_base(scanout_page);
    if (!scanout){
        g_display_gpu_failure_count++;
        return -1;
    }
    s->scanout_page = scanout_page;

    display_copy_surface(scanout,
                         s->pitch,
                         s->framebuffer,
                         s->pitch,
                         s->width,
                         s->height);
    s->framebuffer = scanout;
    s->framebuffer_size = display_fb_size();
    s->scanout_attached = 1;
    display_invalidate_cursor_locked();
    return 0;
}

static void display_init_text_locked(void){
    display_session_t* s = &g_display_sessions[DISPLAY_TEXT_SESSION_ID];
    s->session_id = DISPLAY_TEXT_SESSION_ID;
    s->owner_pid = -1;
    s->type = DISPLAY_TEXT;
    s->active = 1;
    s->framebuffer = (void*)fb_get_base();
    s->framebuffer_size = display_fb_size();
    s->backing_framebuffer = s->framebuffer;
    s->backing_framebuffer_size = s->framebuffer_size;
    s->allocation = 0;
    s->allocation_size = 0UL;
    s->dirty = 1u;
    s->width = fb_get_width();
    s->height = fb_get_height();
    s->pitch = fb_get_pitch();
    display_mark_full_dirty_locked(s);
    g_display_active = DISPLAY_TEXT_SESSION_ID;
}

static int display_get_or_create_graphics_for_pid(int owner_pid){
    if (owner_pid < 0){
        return -1;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int session_id = display_find_graphics_for_pid_locked(owner_pid);
    spin_unlock_irqrestore(&g_display_lock, irq);
    if (session_id >= 0){
        return session_id;
    }

    session_id = display_create_graphics_session(owner_pid);
    return session_id;
}

void display_init(void){
    if (g_display_ready){
        return;
    }

    spinlock_init(&g_display_lock);
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    for (int i = 0; i < DISPLAY_MAX_SESSIONS; i++){
        display_clear_session_locked(i);
    }
    g_display_pending_switch_pid = -1;
    display_init_text_locked();
    g_display_ready = 1;
    spin_unlock_irqrestore(&g_display_lock, irq);
}

int display_set_text_owner(int owner_pid){
    display_init();
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    g_display_sessions[DISPLAY_TEXT_SESSION_ID].owner_pid = owner_pid;
    spin_unlock_irqrestore(&g_display_lock, irq);
    return DISPLAY_TEXT_SESSION_ID;
}

int display_create_graphics_session(int owner_pid){
    if (owner_pid < 0){
        return -1;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int existing = display_find_graphics_for_pid_locked(owner_pid);
    if (existing >= 0){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return existing;
    }
    spin_unlock_irqrestore(&g_display_lock, irq);

    unsigned long size = display_fb_size();
    if (size == 0UL){
        return -1;
    }

    unsigned long alloc_size = size + DISPLAY_FRAMEBUFFER_ALIGN;
    if (alloc_size < size){
        return -1;
    }
    void* raw_fb = kmalloc(alloc_size);
    if (!raw_fb){
        return -1;
    }
    void* fb = (void*)display_align_up((unsigned long)raw_fb,
                                       DISPLAY_FRAMEBUFFER_ALIGN);
    display_memset(fb, 0, size);

    irq = spin_lock_irqsave(&g_display_lock);
    existing = display_find_graphics_for_pid_locked(owner_pid);
    if (existing >= 0){
        spin_unlock_irqrestore(&g_display_lock, irq);
        kfree_secure(raw_fb, alloc_size);
        return existing;
    }

    int slot = -1;
    for (int i = 1; i < DISPLAY_MAX_SESSIONS; i++){
        if (g_display_sessions[i].type == DISPLAY_NONE){
            slot = i;
            break;
        }
    }
    if (slot < 0){
        spin_unlock_irqrestore(&g_display_lock, irq);
        kfree_secure(raw_fb, alloc_size);
        return -1;
    }

    g_display_sessions[slot].session_id = slot;
    g_display_sessions[slot].owner_pid = owner_pid;
    g_display_sessions[slot].type = DISPLAY_GRAPHICS;
    g_display_sessions[slot].active = 0;
    g_display_sessions[slot].framebuffer = fb;
    g_display_sessions[slot].framebuffer_size = size;
    g_display_sessions[slot].backing_framebuffer = fb;
    g_display_sessions[slot].backing_framebuffer_size = size;
    g_display_sessions[slot].allocation = raw_fb;
    g_display_sessions[slot].allocation_size = alloc_size;
    g_display_sessions[slot].dirty = 0u;
    g_display_sessions[slot].dirty_x0 = 0u;
    g_display_sessions[slot].dirty_y0 = 0u;
    g_display_sessions[slot].dirty_x1 = 0u;
    g_display_sessions[slot].dirty_y1 = 0u;
    g_display_sessions[slot].width = fb_get_width();
    g_display_sessions[slot].height = fb_get_height();
    g_display_sessions[slot].pitch = fb_get_pitch();
    g_display_sessions[slot].scanout_attached = 0;
    g_display_sessions[slot].scanout_page = 0u;
    display_mark_full_dirty_locked(&g_display_sessions[slot]);
    spin_unlock_irqrestore(&g_display_lock, irq);
    return slot;
}

int display_destroy_session(int session_id){
    if (!display_valid_id(session_id) || session_id == DISPLAY_TEXT_SESSION_ID){
        return -1;
    }

    display_init();

    void* fb = 0;
    unsigned long size = 0UL;
    int switched_active = 0;
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    if (g_display_sessions[session_id].type == DISPLAY_NONE){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    display_detach_scanout_locked(&g_display_sessions[session_id]);
    fb = g_display_sessions[session_id].allocation ?
         g_display_sessions[session_id].allocation :
         g_display_sessions[session_id].framebuffer;
    size = g_display_sessions[session_id].allocation ?
           g_display_sessions[session_id].allocation_size :
           g_display_sessions[session_id].framebuffer_size;
    display_clear_session_locked(session_id);
    if (g_display_active == session_id){
        display_init_text_locked();
        switched_active = 1;
    }
    spin_unlock_irqrestore(&g_display_lock, irq);

    if (switched_active){
        (void)fb_set_display_page(0u);
        usb_host_flush_input();
    }
    if (fb && size > 0UL){
        kfree_secure(fb, size);
    }
    return 0;
}

void display_destroy_for_pid(int owner_pid){
    if (owner_pid < 0){
        return;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    if (g_display_sessions[DISPLAY_TEXT_SESSION_ID].owner_pid == owner_pid){
        g_display_sessions[DISPLAY_TEXT_SESSION_ID].owner_pid = -1;
    }
    if (g_display_pending_switch_pid == owner_pid){
        g_display_pending_switch_pid = -1;
    }
    spin_unlock_irqrestore(&g_display_lock, irq);

    for (int i = 1; i < DISPLAY_MAX_SESSIONS; i++){
        int destroy = 0;
        irq = spin_lock_irqsave(&g_display_lock);
        destroy = (g_display_sessions[i].type == DISPLAY_GRAPHICS &&
                   g_display_sessions[i].owner_pid == owner_pid);
        spin_unlock_irqrestore(&g_display_lock, irq);
        if (destroy){
            (void)display_destroy_session(i);
        }
    }
}

int display_set_active(int session_id){
    if (!display_valid_id(session_id)){
        return -1;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int old_active = g_display_active;
    if (g_display_sessions[session_id].type == DISPLAY_NONE){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    if (display_valid_id(old_active) && old_active != session_id &&
        g_display_sessions[old_active].type == DISPLAY_GRAPHICS){
        display_detach_scanout_locked(&g_display_sessions[old_active]);
    }
    for (int i = 0; i < DISPLAY_MAX_SESSIONS; i++){
        g_display_sessions[i].active = 0;
    }
    g_display_sessions[session_id].active = 1;
    if (session_id != DISPLAY_TEXT_SESSION_ID){
        display_mark_full_dirty_locked(&g_display_sessions[session_id]);
    } else{
        g_display_sessions[session_id].dirty = 1u;
    }
    g_display_active = session_id;
    g_display_pending_switch_pid = -1;
    display_invalidate_cursor_locked();
    spin_unlock_irqrestore(&g_display_lock, irq);
    if (old_active != session_id){
        usb_host_flush_input();
    }
    if (session_id == DISPLAY_TEXT_SESSION_ID){
        (void)fb_set_display_page(0u);
    }
    if (session_id != DISPLAY_TEXT_SESSION_ID){
        (void)display_present_active_graphics();
    }
    return 0;
}

int display_set_active_for_pid(int owner_pid){
    if (owner_pid < 0){
        return -1;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int session_id = display_find_graphics_for_pid_locked(owner_pid);
    if (session_id < 0){
        // The process may not have issued its first framebuffer syscall yet.
        // Remember the requested owner and switch on its first present().
        g_display_pending_switch_pid = owner_pid;
    }
    spin_unlock_irqrestore(&g_display_lock, irq);
    if (session_id < 0){
        return 0;
    }
    return display_set_active(session_id);
}

int display_get_active(void){
    display_init();
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int active = g_display_active;
    spin_unlock_irqrestore(&g_display_lock, irq);
    return active;
}

int display_get_for_pid(int owner_pid){
    if (owner_pid < 0){
        return -1;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    for (int i = 0; i < DISPLAY_MAX_SESSIONS; i++){
        if (g_display_sessions[i].type != DISPLAY_NONE &&
            g_display_sessions[i].owner_pid == owner_pid){
            spin_unlock_irqrestore(&g_display_lock, irq);
            return i;
        }
    }
    spin_unlock_irqrestore(&g_display_lock, irq);
    return -1;
}

int display_get_info(int session_id, display_session_t* out){
    if (!display_valid_id(session_id) || !out){
        return -1;
    }

    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    if (g_display_sessions[session_id].type == DISPLAY_NONE){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    *out = g_display_sessions[session_id];
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}

void* display_get_framebuffer_for_pid(int owner_pid){
    int session_id = display_get_for_pid(owner_pid);
    if (session_id < 0){
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    void* fb = g_display_sessions[session_id].framebuffer;
    spin_unlock_irqrestore(&g_display_lock, irq);
    return fb;
}

unsigned long display_get_framebuffer_size_for_pid(int owner_pid){
    int session_id = display_get_for_pid(owner_pid);
    if (session_id < 0){
        return 0UL;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    unsigned long size = g_display_sessions[session_id].framebuffer_size;
    spin_unlock_irqrestore(&g_display_lock, irq);
    return size;
}

int display_mark_dirty_for_pid(int owner_pid){
    int session_id = display_get_for_pid(owner_pid);
    if (session_id < 0){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    display_mark_full_dirty_locked(&g_display_sessions[session_id]);
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}

int display_is_active_graphics_pid(int owner_pid){
    int session_id = display_get_for_pid(owner_pid);
    if (session_id < 0){
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int ok = (g_display_active == session_id &&
              g_display_sessions[session_id].type == DISPLAY_GRAPHICS);
    spin_unlock_irqrestore(&g_display_lock, irq);
    return ok;
}

int display_clear_for_pid(int owner_pid, unsigned int color){
    int session_id = display_get_or_create_graphics_for_pid(owner_pid);
    if (session_id < 0){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    display_session_t* s = &g_display_sessions[session_id];
    if (s->type != DISPLAY_GRAPHICS || !s->framebuffer || s->pitch == 0u ||
        s->pitch < (s->width * sizeof(unsigned int))){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    for (unsigned int y = 0; y < s->height; y++){
        unsigned int* row = (unsigned int*)((unsigned char*)s->framebuffer +
                                            ((unsigned long)y * s->pitch));
        display_fill_u32(row, color, s->width);
    }
    display_mark_full_dirty_locked(s);
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}

int display_rect_for_pid(int owner_pid,
                         unsigned int x,
                         unsigned int y,
                         unsigned int w,
                         unsigned int h,
                         unsigned int color){
    int session_id = display_get_or_create_graphics_for_pid(owner_pid);
    if (session_id < 0 || w == 0u || h == 0u){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    display_session_t* s = &g_display_sessions[session_id];
    if (s->type != DISPLAY_GRAPHICS || !s->framebuffer || s->pitch == 0u ||
        s->pitch < (s->width * sizeof(unsigned int)) ||
        x >= s->width || y >= s->height){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    if (x + w < x || x + w > s->width){
        w = s->width - x;
    }
    if (y + h < y || y + h > s->height){
        h = s->height - y;
    }

    for (unsigned int py = y; py < y + h; py++){
        unsigned int* row = (unsigned int*)((unsigned char*)s->framebuffer +
                                            ((unsigned long)py * s->pitch));
        display_fill_u32(row + x, color, w);
    }
    display_mark_rect_dirty_locked(s, x, y, w, h);
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}

int display_blit_rgba32_for_pid(int owner_pid,
                                unsigned int x,
                                unsigned int y,
                                unsigned int w,
                                unsigned int h,
                                const unsigned char* rgba,
                                unsigned int rgba_pitch){
    int session_id = display_get_or_create_graphics_for_pid(owner_pid);
    if (session_id < 0 || !rgba || w == 0u || h == 0u){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    display_session_t* s = &g_display_sessions[session_id];
    if (s->type != DISPLAY_GRAPHICS || !s->framebuffer || s->pitch == 0u ||
        s->pitch < (s->width * sizeof(unsigned int)) ||
        x >= s->width || y >= s->height){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    if (rgba_pitch < (w * 4u)){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    if (x + w < x || x + w > s->width){
        w = s->width - x;
    }
    if (y + h < y || y + h > s->height){
        h = s->height - y;
    }

    for (unsigned int py = 0u; py < h; py++){
        const unsigned char* src_row = rgba + ((unsigned long)py * rgba_pitch);
        unsigned int* dst_row = (unsigned int*)((unsigned char*)s->framebuffer +
                                                 ((unsigned long)(y + py) * s->pitch));
        dst_row += x;

        for (unsigned int px = 0u; px < w; px++){
            const unsigned int si = px * 4u;
            unsigned int sr = src_row[si + 0u];
            unsigned int sg = src_row[si + 1u];
            unsigned int sb = src_row[si + 2u];
            unsigned int sa = src_row[si + 3u];

            if (sa == 0u){
                continue;
            }

            if (sa >= 255u){
                dst_row[px] = (sr << 16) | (sg << 8) | sb;
                continue;
            }

            unsigned int dc = dst_row[px];
            unsigned int dr = (dc >> 16) & 0xFFu;
            unsigned int dg = (dc >> 8) & 0xFFu;
            unsigned int db = dc & 0xFFu;
            unsigned int ia = 255u - sa;

            unsigned int orv = (sr * sa + dr * ia + 127u) / 255u;
            unsigned int ogv = (sg * sa + dg * ia + 127u) / 255u;
            unsigned int obv = (sb * sa + db * ia + 127u) / 255u;
            dst_row[px] = (orv << 16) | (ogv << 8) | obv;
        }
    }
    display_mark_rect_dirty_locked(s, x, y, w, h);
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}

int display_present_for_pid(int owner_pid){
    int session_id = display_get_or_create_graphics_for_pid(owner_pid);
    if (session_id < 0){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int did_switch = 0;
    if (g_display_sessions[session_id].type != DISPLAY_GRAPHICS){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    if (g_display_pending_switch_pid == owner_pid){
        int old_active = g_display_active;
        if (display_valid_id(old_active) && old_active != session_id &&
            g_display_sessions[old_active].type == DISPLAY_GRAPHICS){
            display_detach_scanout_locked(&g_display_sessions[old_active]);
        }
        for (int i = 0; i < DISPLAY_MAX_SESSIONS; i++){
            g_display_sessions[i].active = 0;
        }
        g_display_sessions[session_id].active = 1;
        g_display_active = session_id;
        g_display_pending_switch_pid = -1;
        did_switch = 1;
        display_mark_full_dirty_locked(&g_display_sessions[session_id]);
        display_invalidate_cursor_locked();
    }
    int active = (g_display_active == session_id);
    spin_unlock_irqrestore(&g_display_lock, irq);

    if (did_switch){
        usb_host_flush_input();
    }
    if (!active){
        return 0;
    }
    return display_present_active_graphics();
}

int display_present_active_graphics(void){
    display_init();
    unsigned long prof_hz = display_read_cntfrq();
    unsigned long prof_start = display_read_cntpct();
    unsigned long poll_start = prof_start;

    /*
     * Graphics apps may render continuously without polling SDL events every
     * frame. Poll the keyboard here so global display hotkeys still work, but
     * do not make every present pay for USB host traffic.
     */
    if (system_ticks >= g_display_next_hotkey_poll_tick){
        usb_host_poll();
        g_display_next_hotkey_poll_tick = system_ticks + DISPLAY_HOTKEY_POLL_MS;
    }
    unsigned long poll_us = display_elapsed_us(poll_start, prof_hz);
    unsigned long attach_us = 0UL;
    unsigned long copy_us = 0UL;
    unsigned long cursor_us = 0UL;
    unsigned long flip_us = 0UL;

    unsigned int visible_page = fb_get_display_page();
    unsigned long dst_base = fb_get_page_base(visible_page);
    if (!dst_base){
        dst_base = fb_get_base();
        visible_page = 0u;
    }
    unsigned int dst_pitch = fb_get_pitch();
    unsigned int dst_width = fb_get_width();
    unsigned int dst_height = fb_get_height();
    if (!dst_base || dst_pitch == 0u || dst_width == 0u || dst_height == 0u){
        return -1;
    }

    usb_mouse_state_t mouse;
    if (usb_host_get_mouse_state(&mouse) != 0){
        mouse.present = 0;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int session_id = g_display_active;
    if (!display_valid_id(session_id) ||
        g_display_sessions[session_id].type != DISPLAY_GRAPHICS ||
        !g_display_sessions[session_id].framebuffer ||
        g_display_sessions[session_id].pitch == 0u){
        g_display_profile.present_calls++;
        g_display_profile.usb_poll_us += poll_us;
        g_display_profile.attach_us += attach_us;
        g_display_profile.copy_us += copy_us;
        g_display_profile.cursor_us += cursor_us;
        g_display_profile.flip_us += flip_us;
        g_display_profile.no_work_calls++;
        unsigned long total_us = display_elapsed_us(prof_start, prof_hz);
        g_display_profile.present_us += total_us;
        if (total_us > g_display_profile.present_max_us){
            g_display_profile.present_max_us = total_us;
        }
        spin_unlock_irqrestore(&g_display_lock, irq);
        return 0;
    }

    display_session_t* s = &g_display_sessions[session_id];
    int pageflip_attached = 0;
    unsigned long attach_start = display_read_cntpct();
    if (g_display_gpu_enabled &&
        display_attach_scanout_locked(s) == 0 &&
        s->scanout_attached){
        pageflip_attached = 1;
    }
    attach_us = display_elapsed_us(attach_start, prof_hz);

    unsigned int copy_x0 = 0u;
    unsigned int copy_y0 = 0u;
    unsigned int copy_x1 = 0u;
    unsigned int copy_y1 = 0u;
    int have_dirty = (s->dirty != 0u) ? 1 : 0;
    if (have_dirty){
        copy_x0 = s->dirty_x0;
        copy_y0 = s->dirty_y0;
        copy_x1 = s->dirty_x1;
        copy_y1 = s->dirty_y1;
        if (copy_x1 > s->width){
            copy_x1 = s->width;
        }
        if (copy_y1 > s->height){
            copy_y1 = s->height;
        }
        if (copy_x1 > dst_width){
            copy_x1 = dst_width;
        }
        if (copy_y1 > dst_height){
            copy_y1 = dst_height;
        }
        if (copy_x0 >= copy_x1 || copy_y0 >= copy_y1){
            have_dirty = 0;
        }
        if (have_dirty && !pageflip_attached && dma_is_enabled()){
            unsigned long dirty_pixels =
                (unsigned long)(copy_x1 - copy_x0) *
                (unsigned long)(copy_y1 - copy_y0);
            unsigned int full_x1 = s->width < dst_width ? s->width : dst_width;
            unsigned int full_y1 = s->height < dst_height ? s->height : dst_height;
            unsigned long full_pixels =
                (unsigned long)full_x1 * (unsigned long)full_y1;

            if (full_pixels > 0ul &&
                (dirty_pixels * DISPLAY_DMA_FULL_FRAME_THRESHOLD_DEN) >=
                (full_pixels * DISPLAY_DMA_FULL_FRAME_THRESHOLD_NUM)){
                /*
                 * Large scrolling/redraw frames are better tested as a single
                 * full-screen present. If pitch is contiguous, the DMA layer
                 * uses one linear transfer; otherwise it uses one 2D control
                 * block, not per-line syscalls.
                 */
                copy_x0 = 0u;
                copy_y0 = 0u;
                copy_x1 = full_x1;
                copy_y1 = full_y1;
            }
        }
    }

    unsigned int full_x1 = s->width < dst_width ? s->width : dst_width;
    unsigned int full_y1 = s->height < dst_height ? s->height : dst_height;
    int dirty_full_frame = (have_dirty &&
                            copy_x0 == 0u &&
                            copy_y0 == 0u &&
                            copy_x1 >= full_x1 &&
                            copy_y1 >= full_y1) ? 1 : 0;
    int pageflip_present = (pageflip_attached &&
                            dirty_full_frame &&
                            fb_get_page_base(s->scanout_page) != 0UL) ? 1 : 0;
    if (pageflip_present){
        dst_base = fb_get_page_base(s->scanout_page);
    }

    int had_cursor = (g_cursor_drawn && g_cursor_session_id == session_id) ? 1 : 0;
    int want_cursor = mouse.present ? 1 : 0;
    int cursor_changed = 0;
    if (want_cursor){
        cursor_changed = (!had_cursor ||
                          g_cursor_seq != mouse.seq ||
                          g_cursor_x != mouse.x ||
                          g_cursor_y != mouse.y);
    }
    int cursor_removed = (had_cursor && !want_cursor) ? 1 : 0;
    int need_page_flip = (pageflip_present &&
                          fb_get_display_page() != s->scanout_page) ? 1 : 0;

    if (!have_dirty && !cursor_changed && !cursor_removed && !need_page_flip){
        g_display_profile.present_calls++;
        g_display_profile.usb_poll_us += poll_us;
        g_display_profile.attach_us += attach_us;
        g_display_profile.copy_us += copy_us;
        g_display_profile.cursor_us += cursor_us;
        g_display_profile.flip_us += flip_us;
        g_display_profile.no_work_calls++;
        unsigned long total_us = display_elapsed_us(prof_start, prof_hz);
        g_display_profile.present_us += total_us;
        if (total_us > g_display_profile.present_max_us){
            g_display_profile.present_max_us = total_us;
        }
        spin_unlock_irqrestore(&g_display_lock, irq);
        return 0;
    }

    if (have_dirty && !pageflip_present){
        unsigned long copy_start = display_read_cntpct();
        (void)display_copy_rect_locked(s,
                                       dst_base,
                                       dst_pitch,
                                       dst_width,
                                       dst_height,
                                       copy_x0,
                                       copy_y0,
                                       copy_x1,
                                       copy_y1);
        copy_us += display_elapsed_us(copy_start, prof_hz);
        g_display_profile.buffered_calls++;
        g_display_profile.copied_pixels +=
            (unsigned long)(copy_x1 - copy_x0) *
            (unsigned long)(copy_y1 - copy_y0);
    }

    unsigned long cursor_start = display_read_cntpct();
    if (had_cursor && (cursor_changed || cursor_removed)){
        unsigned int ox0 = 0u, oy0 = 0u, ox1 = 0u, oy1 = 0u;
        if (display_cursor_rect(g_cursor_x,
                                g_cursor_y,
                                dst_width,
                                dst_height,
                                &ox0,
                                &oy0,
                                &ox1,
                                &oy1) == 0){
            int old_covered = have_dirty &&
                              display_rect_contains(copy_x0, copy_y0, copy_x1, copy_y1,
                                                    ox0, oy0, ox1, oy1);
            if (!old_covered){
                display_restore_cursor_saved_locked(dst_base,
                                                    dst_pitch,
                                                    dst_width,
                                                    dst_height,
                                                    session_id);
            } else{
                g_cursor_saved_valid = 0;
            }
        }
    }

    if (want_cursor){
        if (have_dirty || cursor_changed){
            unsigned int nx0 = 0u, ny0 = 0u, nx1 = 0u, ny1 = 0u;
            if (display_cursor_rect(mouse.x,
                                    mouse.y,
                                    dst_width,
                                    dst_height,
                                    &nx0,
                                    &ny0,
                                    &nx1,
                                    &ny1) == 0){
                if (!pageflip_present &&
                    (!have_dirty ||
                    !display_rect_contains(copy_x0, copy_y0, copy_x1, copy_y1,
                                           nx0, ny0, nx1, ny1))){
                    (void)display_copy_rect_locked(s,
                                                   dst_base,
                                                   dst_pitch,
                                                   dst_width,
                                                   dst_height,
                                                   nx0,
                                                   ny0,
                                                   nx1,
                                                   ny1);
                }
                display_save_cursor_under_locked(dst_base,
                                                 dst_pitch,
                                                 dst_width,
                                                 dst_height,
                                                 session_id,
                                                 nx0,
                                                 ny0,
                                                 nx1,
                                                 ny1);
            }
            display_draw_cursor_overlay(dst_base,
                                        dst_pitch,
                                        dst_width,
                                        dst_height,
                                        mouse.x,
                                        mouse.y,
                                        mouse.buttons);
            g_cursor_drawn = 1;
            g_cursor_session_id = session_id;
            g_cursor_x = mouse.x;
            g_cursor_y = mouse.y;
            g_cursor_seq = mouse.seq;
        }
    } else if (had_cursor){
        display_restore_cursor_saved_locked(dst_base,
                                            dst_pitch,
                                            dst_width,
                                            dst_height,
                                            session_id);
        display_invalidate_cursor_locked();
    }
    cursor_us = display_elapsed_us(cursor_start, prof_hz);

    if (pageflip_present && fb_get_display_page() != s->scanout_page){
        unsigned int previous_visible_page = fb_get_display_page();
        unsigned int presented_page = s->scanout_page;
        unsigned long flip_start = display_read_cntpct();
        /*
         * Circle's framebuffer path uses firmware VSYNC waiting with virtual
         * offset flips. On Pi 3 we often only get two pages, so syncing the
         * offset update to vblank is the best available anti-tear guard.
         */
        (void)fb_wait_vsync();
        if (fb_set_display_page(s->scanout_page) == 0){
            flip_us += display_elapsed_us(flip_start, prof_hz);
            g_display_gpu_flip_count++;
            g_display_profile.pageflip_calls++;
            unsigned int next_page = display_choose_scanout_page(presented_page,
                                                                 previous_visible_page,
                                                                 fb_get_page_count() > 2u);
            if (next_page >= fb_get_page_count()){
                next_page = display_choose_scanout_page(presented_page,
                                                       presented_page,
                                                       0);
            }
            unsigned long next_base = fb_get_page_base(next_page);
            if (next_base){
                s->framebuffer = (void*)next_base;
                s->framebuffer_size = display_fb_size();
                s->scanout_page = next_page;
            } else{
                g_display_gpu_failure_count++;
            }
        } else{
            flip_us += display_elapsed_us(flip_start, prof_hz);
            g_display_gpu_failure_count++;
        }
    }

    s->dirty = 0u;
    s->dirty_x0 = 0u;
    s->dirty_y0 = 0u;
    s->dirty_x1 = 0u;
    s->dirty_y1 = 0u;
    g_display_profile.present_calls++;
    g_display_profile.usb_poll_us += poll_us;
    g_display_profile.attach_us += attach_us;
    g_display_profile.copy_us += copy_us;
    g_display_profile.cursor_us += cursor_us;
    g_display_profile.flip_us += flip_us;
    if (!have_dirty && (cursor_changed || cursor_removed)){
        g_display_profile.cursor_only_calls++;
    }
    if (dirty_full_frame){
        g_display_profile.full_dirty_calls++;
    } else if (have_dirty){
        g_display_profile.partial_dirty_calls++;
    }
    {
        unsigned long total_us = display_elapsed_us(prof_start, prof_hz);
        g_display_profile.present_us += total_us;
        if (total_us > g_display_profile.present_max_us){
            g_display_profile.present_max_us = total_us;
        }
    }
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}

int display_present_active(void){
    return display_present_active_graphics();
}

int display_gpu_set_enabled(int enabled){
    display_init();

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    g_display_gpu_enabled = enabled ? 1 : 0;
    if (!g_display_gpu_enabled){
        for (int i = 1; i < DISPLAY_MAX_SESSIONS; i++){
            if (g_display_sessions[i].type == DISPLAY_GRAPHICS){
                display_detach_scanout_locked(&g_display_sessions[i]);
            }
        }
        display_invalidate_cursor_locked();
    } else if (!display_pageflip_available()){
        g_display_gpu_enabled = 0;
        g_display_gpu_failure_count++;
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    spin_unlock_irqrestore(&g_display_lock, irq);

    if (!enabled){
        (void)fb_set_display_page(0u);
        (void)display_present_active_graphics();
    } else if (display_get_active() != DISPLAY_TEXT_SESSION_ID){
        (void)display_present_active_graphics();
    }
    return 0;
}

unsigned int display_gpu_status(void){
    unsigned int st = 0u;
    if (g_display_gpu_enabled){
        st |= 1u;
    }
    if (display_pageflip_available()){
        st |= 2u;
    }
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int active = g_display_active;
    if (display_valid_id(active) && g_display_sessions[active].scanout_attached){
        st |= 4u;
    }
    spin_unlock_irqrestore(&g_display_lock, irq);
    st |= (fb_get_page_count() & 0xFFu) << 8;
    st |= (g_display_gpu_failure_count & 0xFFFFu) << 16;
    return st;
}

unsigned int display_gpu_flip_count(void){
    return g_display_gpu_flip_count;
}

unsigned int display_gpu_failure_count(void){
    return g_display_gpu_failure_count;
}

void display_profile_reset(void){
    display_init();
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    g_display_profile.present_calls = 0UL;
    g_display_profile.pageflip_calls = 0UL;
    g_display_profile.buffered_calls = 0UL;
    g_display_profile.cursor_only_calls = 0UL;
    g_display_profile.no_work_calls = 0UL;
    g_display_profile.full_dirty_calls = 0UL;
    g_display_profile.partial_dirty_calls = 0UL;
    g_display_profile.copied_pixels = 0UL;
    g_display_profile.present_us = 0UL;
    g_display_profile.present_max_us = 0UL;
    g_display_profile.usb_poll_us = 0UL;
    g_display_profile.attach_us = 0UL;
    g_display_profile.copy_us = 0UL;
    g_display_profile.cursor_us = 0UL;
    g_display_profile.flip_us = 0UL;
    spin_unlock_irqrestore(&g_display_lock, irq);
}

void display_profile_dump(void){
    display_profile_t snap;
    display_init();
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    snap = g_display_profile;
    spin_unlock_irqrestore(&g_display_lock, irq);

    klog_puts("DISPLAY profile: present=");
    klog_putdec(snap.present_calls);
    klog_puts(" pageflip=");
    klog_putdec(snap.pageflip_calls);
    klog_puts(" buffered=");
    klog_putdec(snap.buffered_calls);
    klog_puts(" cursor=");
    klog_putdec(snap.cursor_only_calls);
    klog_puts(" nowork=");
    klog_putdec(snap.no_work_calls);
    klog_puts("\n");

    klog_puts("DISPLAY dirty: full=");
    klog_putdec(snap.full_dirty_calls);
    klog_puts(" partial=");
    klog_putdec(snap.partial_dirty_calls);
    klog_puts(" copied_pixels=");
    klog_putdec(snap.copied_pixels);
    klog_puts("\n");

    klog_puts("DISPLAY us: present_total=");
    klog_putdec(snap.present_us);
    klog_puts(" max=");
    klog_putdec(snap.present_max_us);
    klog_puts(" usb_poll=");
    klog_putdec(snap.usb_poll_us);
    klog_puts("\n");

    klog_puts("DISPLAY us2: attach=");
    klog_putdec(snap.attach_us);
    klog_puts(" copy=");
    klog_putdec(snap.copy_us);
    klog_puts(" cursor=");
    klog_putdec(snap.cursor_us);
    klog_puts(" flip=");
    klog_putdec(snap.flip_us);
    klog_puts("\n");
}
