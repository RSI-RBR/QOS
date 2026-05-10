#include "gpu2d.h"
#include "display.h"
#include "v3d.h"

static unsigned int g_gpu2d_blit_count = 0u;
static unsigned int g_gpu2d_clear_count = 0u;
static unsigned int g_gpu2d_fill_count = 0u;
static unsigned int g_gpu2d_fallback_count = 0u;
static unsigned int g_gpu2d_unsupported_count = 0u;

unsigned int gpu2d_status(void){
    qos_v3d_status_t st;
    unsigned int status = QOS_GPU2D_STATUS_READY |
                          QOS_GPU2D_STATUS_BACKEND_SOFT |
                          QOS_GPU2D_CAP_SOFTWARE_FALLBACK;
    if (v3d_get_status(&st) == 0 &&
        (st.flags & QOS_V3D_FLAG_SCRATCH_OK)){
        status |= QOS_GPU2D_STATUS_BACKEND_HW |
                  QOS_GPU2D_CAP_ACCEL_CLEAR |
                  QOS_GPU2D_CAP_ACCEL_FILL_TILE;
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
