#include "display.h"
#include "framebuffer.h"
#include "memory.h"
#include "spinlock.h"

static display_session_t g_display_sessions[DISPLAY_MAX_SESSIONS];
static spinlock_t g_display_lock;
static int g_display_ready = 0;
static int g_display_active = DISPLAY_TEXT_SESSION_ID;

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
    g_display_sessions[session_id].dirty = 0u;
    g_display_sessions[session_id].dirty_x0 = 0u;
    g_display_sessions[session_id].dirty_y0 = 0u;
    g_display_sessions[session_id].dirty_x1 = 0u;
    g_display_sessions[session_id].dirty_y1 = 0u;
    g_display_sessions[session_id].width = 0u;
    g_display_sessions[session_id].height = 0u;
    g_display_sessions[session_id].pitch = 0u;
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

static void display_init_text_locked(void){
    display_session_t* s = &g_display_sessions[DISPLAY_TEXT_SESSION_ID];
    s->session_id = DISPLAY_TEXT_SESSION_ID;
    s->owner_pid = -1;
    s->type = DISPLAY_TEXT;
    s->active = 1;
    s->framebuffer = (void*)fb_get_base();
    s->framebuffer_size = display_fb_size();
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

    void* fb = kmalloc(size);
    if (!fb){
        return -1;
    }
    display_memset(fb, 0, size);

    irq = spin_lock_irqsave(&g_display_lock);
    existing = display_find_graphics_for_pid_locked(owner_pid);
    if (existing >= 0){
        spin_unlock_irqrestore(&g_display_lock, irq);
        kfree_secure(fb, size);
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
        kfree_secure(fb, size);
        return -1;
    }

    g_display_sessions[slot].session_id = slot;
    g_display_sessions[slot].owner_pid = owner_pid;
    g_display_sessions[slot].type = DISPLAY_GRAPHICS;
    g_display_sessions[slot].active = 0;
    g_display_sessions[slot].framebuffer = fb;
    g_display_sessions[slot].framebuffer_size = size;
    g_display_sessions[slot].dirty = 0u;
    g_display_sessions[slot].dirty_x0 = 0u;
    g_display_sessions[slot].dirty_y0 = 0u;
    g_display_sessions[slot].dirty_x1 = 0u;
    g_display_sessions[slot].dirty_y1 = 0u;
    g_display_sessions[slot].width = fb_get_width();
    g_display_sessions[slot].height = fb_get_height();
    g_display_sessions[slot].pitch = fb_get_pitch();
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
    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    if (g_display_sessions[session_id].type == DISPLAY_NONE){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    fb = g_display_sessions[session_id].framebuffer;
    size = g_display_sessions[session_id].framebuffer_size;
    display_clear_session_locked(session_id);
    if (g_display_active == session_id){
        display_init_text_locked();
    }
    spin_unlock_irqrestore(&g_display_lock, irq);

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
    if (g_display_sessions[session_id].type == DISPLAY_NONE){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
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
    spin_unlock_irqrestore(&g_display_lock, irq);
    if (session_id != DISPLAY_TEXT_SESSION_ID){
        (void)display_present_active();
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
    spin_unlock_irqrestore(&g_display_lock, irq);
    if (session_id < 0){
        return -1;
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
        for (unsigned int x = 0; x < s->width; x++){
            row[x] = color;
        }
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
        for (unsigned int px = x; px < x + w; px++){
            row[px] = color;
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
    if (g_display_sessions[session_id].type != DISPLAY_GRAPHICS){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    int active = (g_display_active == session_id);
    spin_unlock_irqrestore(&g_display_lock, irq);

    if (!active){
        return 0;
    }
    return display_present_active();
}

int display_present_active(void){
    display_init();

    unsigned long dst_base = fb_get_base();
    unsigned int dst_pitch = fb_get_pitch();
    unsigned int dst_width = fb_get_width();
    unsigned int dst_height = fb_get_height();
    if (!dst_base || dst_pitch == 0u || dst_width == 0u || dst_height == 0u){
        return -1;
    }

    unsigned long irq = spin_lock_irqsave(&g_display_lock);
    int session_id = g_display_active;
    if (!display_valid_id(session_id) ||
        g_display_sessions[session_id].type != DISPLAY_GRAPHICS ||
        !g_display_sessions[session_id].framebuffer ||
        g_display_sessions[session_id].pitch == 0u ||
        g_display_sessions[session_id].dirty == 0u){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return 0;
    }

    display_session_t* s = &g_display_sessions[session_id];
    unsigned int copy_x0 = s->dirty_x0;
    unsigned int copy_y0 = s->dirty_y0;
    unsigned int copy_x1 = s->dirty_x1;
    unsigned int copy_y1 = s->dirty_y1;
    if (copy_x1 > s->width){
        copy_x1 = s->width;
    }
    if (copy_y1 > s->height){
        copy_y1 = s->height;
    }
    if (copy_x0 >= dst_width || copy_y0 >= dst_height){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    if (copy_x1 > dst_width){
        copy_x1 = dst_width;
    }
    if (copy_y1 > dst_height){
        copy_y1 = dst_height;
    }
    if (copy_x0 >= copy_x1 || copy_y0 >= copy_y1){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    unsigned int copy_width = copy_x1 - copy_x0;
    unsigned int copy_height = copy_y1 - copy_y0;
    if (copy_width == 0u || copy_height == 0u){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }

    unsigned int row_bytes = copy_width * sizeof(unsigned int);
    unsigned int row_offset = copy_x0 * sizeof(unsigned int);
    if (dst_pitch < row_offset || s->pitch < row_offset ||
        dst_pitch - row_offset < row_bytes ||
        s->pitch - row_offset < row_bytes){
        spin_unlock_irqrestore(&g_display_lock, irq);
        return -1;
    }
    for (unsigned int y = 0; y < copy_height; y++){
        unsigned int* src = (unsigned int*)((unsigned char*)s->framebuffer +
                                            ((unsigned long)(copy_y0 + y) * s->pitch) +
                                            row_offset);
        unsigned int* dst = (unsigned int*)((unsigned char*)dst_base +
                                            ((unsigned long)(copy_y0 + y) * dst_pitch) +
                                            row_offset);
        for (unsigned int x = 0; x < copy_width; x++){
            dst[x] = src[x];
        }
    }
    s->dirty = 0u;
    s->dirty_x0 = 0u;
    s->dirty_y0 = 0u;
    s->dirty_x1 = 0u;
    s->dirty_y1 = 0u;
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
}
