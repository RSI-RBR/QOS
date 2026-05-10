#include "gpu2d.h"
#include "display.h"
#include "cache.h"
#include "memory.h"
#include "process.h"
#include "spinlock.h"
#include "v3d.h"

#define GPU2D_MAX_TEXTURES 96u
#define GPU2D_TEXTURE_MAX_BYTES (4u * 1024u * 1024u)
#define GPU2D_TEXTURE_TOTAL_MAX_BYTES (128u * 1024u * 1024u)

typedef struct {
    int used;
    int pid;
    unsigned int id;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int flags;
    unsigned long bytes;
    unsigned int* pixels;
} gpu2d_texture_t;

static unsigned int g_gpu2d_blit_count = 0u;
static unsigned int g_gpu2d_clear_count = 0u;
static unsigned int g_gpu2d_fill_count = 0u;
static unsigned int g_gpu2d_quad_count = 0u;
static unsigned int g_gpu2d_quad_batch_count = 0u;
static unsigned int g_gpu2d_fallback_count = 0u;
static unsigned int g_gpu2d_unsupported_count = 0u;
static unsigned int g_gpu2d_texture_upload_count = 0u;
static unsigned int g_gpu2d_texture_free_count = 0u;
static unsigned int g_gpu2d_next_texture_id = 1u;
static unsigned int g_gpu2d_texture_live_count = 0u;
static unsigned int g_gpu2d_texture_live_bytes = 0u;
static gpu2d_texture_t g_gpu2d_textures[GPU2D_MAX_TEXTURES];
static spinlock_t g_gpu2d_lock = {0};

static unsigned int gpu2d_next_id_locked(void){
    unsigned int id = g_gpu2d_next_texture_id++;
    if (id == 0u){
        id = g_gpu2d_next_texture_id++;
    }
    return id;
}

unsigned int gpu2d_status(void){
    qos_v3d_status_t st;
    unsigned int status = QOS_GPU2D_STATUS_READY |
                          QOS_GPU2D_STATUS_BACKEND_SOFT |
                          QOS_GPU2D_CAP_SOFTWARE_FALLBACK |
                          QOS_GPU2D_CAP_QUAD_BATCH;
    if (v3d_get_status(&st) != 0 ||
        (st.flags & QOS_V3D_FLAG_SCRATCH_OK) == 0u){
        /*
         * User programs often query GPU2D capabilities while loading textures,
         * before any explicit V3D command has warmed the hardware. Probe here
         * so "v3d probe" remains diagnostic instead of a required pre-game
         * shell command.
         */
        (void)v3d_probe(&st);
    }
    if ((st.flags & QOS_V3D_FLAG_SCRATCH_OK) != 0u){
        status |= QOS_GPU2D_STATUS_BACKEND_HW |
                  QOS_GPU2D_CAP_ACCEL_CLEAR |
                  QOS_GPU2D_CAP_ACCEL_FILL_TILE |
                  QOS_GPU2D_CAP_TEXTURE_OBJECTS;
    }
    return status;
}

unsigned int gpu2d_blit_count(void){
    return g_gpu2d_blit_count;
}

unsigned int gpu2d_clear_count(void){
    return g_gpu2d_clear_count;
}

unsigned int gpu2d_fill_count(void){
    return g_gpu2d_fill_count;
}

unsigned int gpu2d_quad_count(void){
    return g_gpu2d_quad_count;
}

unsigned int gpu2d_quad_batch_count(void){
    return g_gpu2d_quad_batch_count;
}

unsigned int gpu2d_texture_count(void){
    return g_gpu2d_texture_live_count;
}

unsigned int gpu2d_texture_upload_count(void){
    return g_gpu2d_texture_upload_count;
}

unsigned int gpu2d_texture_free_count(void){
    return g_gpu2d_texture_free_count;
}

unsigned int gpu2d_texture_bytes(void){
    return g_gpu2d_texture_live_bytes;
}

unsigned int gpu2d_fallback_count(void){
    return g_gpu2d_fallback_count;
}

unsigned int gpu2d_unsupported_count(void){
    return g_gpu2d_unsupported_count;
}

int gpu2d_blit_rgba_for_pid(int pid, const qos_gpu2d_blit_t* blit){
    if (!blit || !blit->pixels || blit->src_w == 0u || blit->src_h == 0u ||
        blit->dst_w == 0u || blit->dst_h == 0u ||
        blit->texture_w > 0x3FFFFFFFu ||
        blit->pitch < blit->texture_w * 4u){
        return -1;
    }

    /*
     * Until a hardware backend lands, only expose a conservative software
     * fallback for exact-size RGBA blits. SDL does not use this path unless a
     * future backend reports QOS_GPU2D_CAP_ACCEL_BLIT, so this is mostly for
     * explicit tests and to keep the ABI behavior defined.
     */
    if ((blit->flags & QOS_GPU2D_BLIT_ALLOW_SOFTWARE) == 0u){
        g_gpu2d_unsupported_count++;
        return -2;
    }
    if (blit->dst_x < 0 || blit->dst_y < 0 ||
        blit->src_x + blit->src_w < blit->src_x ||
        blit->src_y + blit->src_h < blit->src_y ||
        blit->src_x + blit->src_w > blit->texture_w ||
        blit->src_y + blit->src_h > blit->texture_h ||
        blit->src_w != blit->dst_w ||
        blit->src_h != blit->dst_h ||
        (blit->flags & (QOS_GPU2D_BLIT_FLIP_X | QOS_GPU2D_BLIT_FLIP_Y)) != 0u){
        g_gpu2d_unsupported_count++;
        return -2;
    }

    const unsigned char* src = blit->pixels +
        ((unsigned long)blit->src_y * (unsigned long)blit->pitch) +
        ((unsigned long)blit->src_x * 4ul);
    int rc = display_blit_rgba32_for_pid(pid,
                                         (unsigned int)blit->dst_x,
                                         (unsigned int)blit->dst_y,
                                         blit->src_w,
                                         blit->src_h,
                                         src,
                                         blit->pitch);
    if (rc == 0){
        g_gpu2d_blit_count++;
        g_gpu2d_fallback_count++;
    }
    return rc;
}

int gpu2d_clear_for_pid(int pid, unsigned int color){
    int session_id = display_get_for_pid(pid);
    display_session_t info;
    if (session_id < 0 || display_get_info(session_id, &info) != 0){
        return -1;
    }
    if (info.type != DISPLAY_GRAPHICS || !info.direct_framebuffer ||
        info.direct_page >= 4u){
        g_gpu2d_unsupported_count++;
        return -2;
    }
    if (v3d_clear_page(info.direct_page, color, 0) != 0){
        g_gpu2d_unsupported_count++;
        return -2;
    }
    g_gpu2d_clear_count++;
    return 0;
}

int gpu2d_fill_rect_for_pid(int pid,
                            unsigned int x,
                            unsigned int y,
                            unsigned int w,
                            unsigned int h,
                            unsigned int color){
    int session_id = display_get_for_pid(pid);
    display_session_t info;
    if (session_id < 0 || display_get_info(session_id, &info) != 0){
        return -1;
    }
    if (info.type != DISPLAY_GRAPHICS || !info.direct_framebuffer ||
        info.direct_page >= 4u ||
        (x & 63u) != 0u || (y & 63u) != 0u ||
        (w & 63u) != 0u || (h & 63u) != 0u ||
        w == 0u || h == 0u ||
        x + w < x || y + h < y ||
        x + w > info.width || y + h > info.height){
        g_gpu2d_unsupported_count++;
        return -2;
    }
    if (v3d_clear_page_tiles(info.direct_page,
                             color,
                             x >> 6,
                             y >> 6,
                             w >> 6,
                             h >> 6,
                             0) != 0){
        g_gpu2d_unsupported_count++;
        return -2;
    }
    g_gpu2d_fill_count++;
    return 0;
}

static int gpu2d_validate_quad_source(const qos_gpu2d_quad_t* quad){
    if (!quad || !quad->src){
        return -1;
    }
    return process_user_range_readable(quad->src, 32u * 32u * sizeof(unsigned int)) ? 0 : -1;
}

int gpu2d_submit_quads_for_pid(int pid,
                               const qos_gpu2d_quad_t* quads,
                               unsigned int count){
    if (pid < 0 || !quads || count == 0u || count > QOS_GPU2D_QUAD_BATCH_MAX){
        return -1;
    }

    for (unsigned int i = 0u; i < count; i++){
        const qos_gpu2d_quad_t* q = &quads[i];
        if ((q->op != QOS_GPU2D_QUAD_FILL32 && q->op != QOS_GPU2D_QUAD_BLIT32) ||
            q->x < 0 || q->y < 0){
            return -1;
        }
        if (q->op == QOS_GPU2D_QUAD_BLIT32 &&
            gpu2d_validate_quad_source(q) != 0){
            return -1;
        }
    }

    unsigned int done = 0u;
    for (unsigned int i = 0u; i < count; i++){
        const qos_gpu2d_quad_t* q = &quads[i];
        int rc;
        if (q->op == QOS_GPU2D_QUAD_FILL32){
            rc = display_rect_for_pid(pid,
                                      (unsigned int)q->x,
                                      (unsigned int)q->y,
                                      32u,
                                      32u,
                                      q->color);
            if (rc == 0){
                g_gpu2d_fill_count++;
            }
        } else{
            rc = display_blit_native32_for_pid(pid,
                                               (unsigned int)q->x,
                                               (unsigned int)q->y,
                                               32u,
                                               32u,
                                               q->src,
                                               32u * sizeof(unsigned int));
            if (rc == 0){
                g_gpu2d_blit_count++;
            }
        }
        if (rc != 0){
            g_gpu2d_unsupported_count++;
            return -2;
        }
        done++;
    }

    g_gpu2d_quad_count += done;
    g_gpu2d_quad_batch_count++;
    g_gpu2d_fallback_count += done;
    return 0;
}

int gpu2d_texture_upload_for_pid(int pid, qos_gpu2d_texture_upload_t* req){
    if (pid < 0 || !req || !req->pixels ||
        req->width == 0u || req->height == 0u ||
        req->width > 4096u || req->height > 4096u ||
        req->pitch != req->width * sizeof(unsigned int)){
        return -1;
    }
    if (req->height > (GPU2D_TEXTURE_MAX_BYTES / req->pitch)){
        return -1;
    }

    unsigned long bytes = (unsigned long)req->pitch * (unsigned long)req->height;
    if (bytes == 0UL || bytes > GPU2D_TEXTURE_MAX_BYTES){
        return -1;
    }
    if (!process_user_range_readable(req->pixels, bytes)){
        return -1;
    }

    unsigned int* pixels = (unsigned int*)kmalloc(bytes);
    if (!pixels){
        return -1;
    }
    if (process_copy_from_user(pixels, req->pixels, bytes) != 0){
        kfree_secure(pixels, bytes);
        return -1;
    }
    clean_data_cache_range((unsigned long)pixels, bytes);

    spin_lock(&g_gpu2d_lock);
    int slot = -1;
    for (unsigned int i = 0u; i < GPU2D_MAX_TEXTURES; i++){
        if (!g_gpu2d_textures[i].used){
            slot = (int)i;
            break;
        }
    }
    if (slot < 0 ||
        (unsigned int)bytes > GPU2D_TEXTURE_TOTAL_MAX_BYTES ||
        g_gpu2d_texture_live_bytes > GPU2D_TEXTURE_TOTAL_MAX_BYTES - (unsigned int)bytes){
        spin_unlock(&g_gpu2d_lock);
        kfree_secure(pixels, bytes);
        return -1;
    }

    unsigned int id = gpu2d_next_id_locked();
    g_gpu2d_textures[slot].used = 1;
    g_gpu2d_textures[slot].pid = pid;
    g_gpu2d_textures[slot].id = id;
    g_gpu2d_textures[slot].width = req->width;
    g_gpu2d_textures[slot].height = req->height;
    g_gpu2d_textures[slot].pitch = req->pitch;
    g_gpu2d_textures[slot].flags = req->flags;
    g_gpu2d_textures[slot].bytes = bytes;
    g_gpu2d_textures[slot].pixels = pixels;
    g_gpu2d_texture_live_count++;
    g_gpu2d_texture_upload_count++;
    if (g_gpu2d_texture_live_bytes <= 0xFFFFFFFFu - (unsigned int)bytes){
        g_gpu2d_texture_live_bytes += (unsigned int)bytes;
    }
    spin_unlock(&g_gpu2d_lock);

    req->texture_id = id;
    return 0;
}

int gpu2d_texture_free_for_pid(int pid, unsigned int texture_id){
    if (pid < 0 || texture_id == 0u){
        return -1;
    }
    spin_lock(&g_gpu2d_lock);
    for (unsigned int i = 0u; i < GPU2D_MAX_TEXTURES; i++){
        gpu2d_texture_t* t = &g_gpu2d_textures[i];
        if (t->used && t->pid == pid && t->id == texture_id){
            unsigned int* pixels = t->pixels;
            unsigned long bytes = t->bytes;
            t->used = 0;
            t->pid = -1;
            t->id = 0u;
            t->pixels = 0;
            t->bytes = 0UL;
            if (g_gpu2d_texture_live_count > 0u){
                g_gpu2d_texture_live_count--;
            }
            if (g_gpu2d_texture_live_bytes >= (unsigned int)bytes){
                g_gpu2d_texture_live_bytes -= (unsigned int)bytes;
            } else{
                g_gpu2d_texture_live_bytes = 0u;
            }
            g_gpu2d_texture_free_count++;
            spin_unlock(&g_gpu2d_lock);
            if (pixels && bytes > 0UL){
                kfree_secure(pixels, bytes);
            }
            return 0;
        }
    }
    spin_unlock(&g_gpu2d_lock);
    return -1;
}

void gpu2d_release_for_pid(int pid){
    if (pid < 0){
        return;
    }
    for (unsigned int i = 0u; i < GPU2D_MAX_TEXTURES; i++){
        unsigned int id = 0u;
        spin_lock(&g_gpu2d_lock);
        if (g_gpu2d_textures[i].used && g_gpu2d_textures[i].pid == pid){
            id = g_gpu2d_textures[i].id;
        }
        spin_unlock(&g_gpu2d_lock);
        if (id){
            (void)gpu2d_texture_free_for_pid(pid, id);
        }
    }
}
