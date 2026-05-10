#include "gpu2d.h"
#include "display.h"

static unsigned int g_gpu2d_blit_count = 0u;
static unsigned int g_gpu2d_fallback_count = 0u;
static unsigned int g_gpu2d_unsupported_count = 0u;

unsigned int gpu2d_status(void){
    /*
     * This is the stable ABI layer for future VideoCore/2D acceleration.
     * Today it intentionally reports no accelerated blit capability, so SDL
     * keeps using its current userspace direct-framebuffer renderer.
     */
    return QOS_GPU2D_STATUS_READY |
           QOS_GPU2D_STATUS_BACKEND_SOFT |
           QOS_GPU2D_CAP_SOFTWARE_FALLBACK;
}

unsigned int gpu2d_blit_count(void){
    return g_gpu2d_blit_count;
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
