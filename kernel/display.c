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
    g_display_sessions[session_id].width = 0u;
    g_display_sessions[session_id].height = 0u;
    g_display_sessions[session_id].pitch = 0u;
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
    g_display_active = DISPLAY_TEXT_SESSION_ID;
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
    for (int i = 1; i < DISPLAY_MAX_SESSIONS; i++){
        if (g_display_sessions[i].type == DISPLAY_GRAPHICS &&
            g_display_sessions[i].owner_pid == owner_pid){
            spin_unlock_irqrestore(&g_display_lock, irq);
            return i;
        }
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
    volatile unsigned char* wipe = (volatile unsigned char*)fb;
    for (unsigned long i = 0; i < size; i++){
        wipe[i] = 0;
    }

    irq = spin_lock_irqsave(&g_display_lock);
    for (int i = 1; i < DISPLAY_MAX_SESSIONS; i++){
        if (g_display_sessions[i].type == DISPLAY_GRAPHICS &&
            g_display_sessions[i].owner_pid == owner_pid){
            spin_unlock_irqrestore(&g_display_lock, irq);
            kfree_secure(fb, size);
            return i;
        }
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
    g_display_sessions[slot].dirty = 1u;
    g_display_sessions[slot].width = fb_get_width();
    g_display_sessions[slot].height = fb_get_height();
    g_display_sessions[slot].pitch = fb_get_pitch();
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
    g_display_sessions[session_id].dirty = 1u;
    g_display_active = session_id;
    spin_unlock_irqrestore(&g_display_lock, irq);
    return 0;
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
    g_display_sessions[session_id].dirty = 1u;
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
