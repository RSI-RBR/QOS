#include "SDL.h"
#include "SDL_image.h"
#include "syscall.h"
#include <stdlib.h>

#define SDL_SHIM_MAX_TEXTURES 128
#define SDL_SHIM_MAX_SURFACES 32
#define SDL_SHIM_ROWBUF_PIXELS 2048
#define SDL_SHIM_BLITBUF_BYTES (512u * 1024u)
#define SDL_SHIM_BMP_FILE_MAX (1024u * 1024u)
#define SDL_SHIM_EVENT_QUEUE_SIZE 64
#define SDL_SHIM_REPEAT_DELAY_MS 400u
#define SDL_SHIM_REPEAT_INTERVAL_MS 33u
#define SDL_SHIM_RENDER_HINT_TILE_FILL 1
#define SDL_SHIM_RENDER_HINT_CHAR_16 2
#define SDL_SHIM_SOFT_BACKBUFFER 1
#define SDL_SHIM_ENABLE_TILE_FILL_FASTPATH 1
#define SDL_SHIM_ENABLE_NATIVE_SCALED_FASTPATH 1
#ifndef SDL_SHIM_ENABLE_GPU2D_TEXTURE_UPLOAD
#define SDL_SHIM_ENABLE_GPU2D_TEXTURE_UPLOAD 1
#endif

static SDL_Window g_window;
static SDL_Renderer g_renderer;
static SDL_Texture g_textures[SDL_SHIM_MAX_TEXTURES];
static SDL_Surface g_surfaces[SDL_SHIM_MAX_SURFACES];
static SDL_Event g_event_queue[SDL_SHIM_EVENT_QUEUE_SIZE];
static int g_event_head = 0;
static int g_event_tail = 0;
static int g_event_count = 0;
static Uint8 g_keyboard_state[SDL_NUM_SCANCODES];
static Uint32 g_key_down_ms[SDL_NUM_SCANCODES];
static Uint32 g_key_repeat_next_ms[SDL_NUM_SCANCODES];
static SDL_Keycode g_key_sym[SDL_NUM_SCANCODES];
static Uint16 g_key_mod[SDL_NUM_SCANCODES];
static Uint16 g_mod_state = KMOD_NONE;
static int g_key_repeat_enabled = 0;
static Uint32 g_key_repeat_delay_ms = SDL_SHIM_REPEAT_DELAY_MS;
static Uint32 g_key_repeat_interval_ms = SDL_SHIM_REPEAT_INTERVAL_MS;
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static int g_mouse_rel_x = 0;
static int g_mouse_rel_y = 0;
static int g_mouse_wheel_x = 0;
static int g_mouse_wheel_y = 0;
static Uint32 g_mouse_buttons = 0u;
static char g_last_error[96] = "OK";
static Uint8 g_rowbuf[SDL_SHIM_ROWBUF_PIXELS * 4u];
static Uint8 g_blitbuf[SDL_SHIM_BLITBUF_BYTES];
static Uint32 g_native_blitbuf[SDL_SHIM_BLITBUF_BYTES / 4u];
static int g_pending_fill_valid = 0;
static unsigned int g_pending_fill_x = 0u;
static unsigned int g_pending_fill_y = 0u;
static unsigned int g_pending_fill_w = 0u;
static unsigned int g_pending_fill_h = 0u;
static unsigned int g_pending_fill_color = 0u;
static Uint8* g_soft_fb = 0;
static int g_soft_fb_w = 0;
static int g_soft_fb_h = 0;
static int g_soft_fb_pitch = 0;
static int g_soft_fb_enabled = 0;
static int g_soft_fb_dirty = 0;
static int g_soft_fb_attached = 0;
static int g_soft_fb_direct = 0;
static int g_soft_fb_direct_inactive = 0;
static int g_soft_fb_direct_checked = 0;

typedef struct sdl_qos_profile {
    Uint64 bmp_calls;
    Uint64 bmp_ok;
    Uint64 bmp_fail;
    Uint64 bmp_bytes;
    Uint64 bmp_read_us;
    Uint64 bmp_decode_us;
    Uint64 bmp_total_us;
    Uint64 bmp_max_us;
    Uint64 texture_calls;
    Uint64 texture_bytes;
    Uint64 texture_us;
    Uint64 rendercopy_calls;
    Uint64 rendercopy_us;
    Uint64 rendercopy_direct_calls;
    Uint64 rendercopy_gpu2d_calls;
    Uint64 rendercopy_gpu2d_miss;
    Uint64 rendercopy_native_calls;
    Uint64 rendercopy_blitbuf_calls;
    Uint64 rendercopy_row_calls;
    Uint64 rendercopy_fill_calls;
    Uint64 rendercopy_soft_calls;
    Uint64 rendercopy_pixels;
    Uint64 fill_calls;
    Uint64 fill_flushes;
    Uint64 fill_pixels;
    Uint64 fill_us;
    Uint64 clear_calls;
    Uint64 clear_us;
    Uint64 poll_calls;
    Uint64 poll_us;
    Uint64 present_calls;
    Uint64 present_us;
    Uint64 present_flush_us;
    Uint64 present_upload_us;
    Uint64 present_kernel_us;
} sdl_qos_profile_t;

static sdl_qos_profile_t g_sdl_profile;
static sdl_qos_profile_t g_sdl_auto_last_profile;
static Uint64 g_sdl_auto_last_us = 0ull;

static void sdl_profile_put_u64(Uint64 v){
    char tmp[32];
    unsigned int n = 0u;
    if (v == 0ull){
        qos_putc('0');
        return;
    }
    while (v && n < (unsigned int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10ull));
        v /= 10ull;
    }
    while (n > 0u){
        qos_putc(tmp[--n]);
    }
}

static void sdl_profile_note_bmp(int ok, int bytes, Uint64 read_us, Uint64 decode_us, Uint64 total_us){
    g_sdl_profile.bmp_calls++;
    if (ok){
        g_sdl_profile.bmp_ok++;
        if (bytes > 0){
            g_sdl_profile.bmp_bytes += (Uint64)bytes;
        }
    } else{
        g_sdl_profile.bmp_fail++;
    }
    g_sdl_profile.bmp_read_us += read_us;
    g_sdl_profile.bmp_decode_us += decode_us;
    g_sdl_profile.bmp_total_us += total_us;
    if (total_us > g_sdl_profile.bmp_max_us){
        g_sdl_profile.bmp_max_us = total_us;
    }
}

static Uint64 sdl_profile_delta(Uint64 now, Uint64 last){
    return (now >= last) ? (now - last) : 0ull;
}

static void sdl_profile_copy(sdl_qos_profile_t* dst, const sdl_qos_profile_t* src){
    if (!dst || !src){
        return;
    }
    dst->bmp_calls = src->bmp_calls;
    dst->bmp_ok = src->bmp_ok;
    dst->bmp_fail = src->bmp_fail;
    dst->bmp_bytes = src->bmp_bytes;
    dst->bmp_read_us = src->bmp_read_us;
    dst->bmp_decode_us = src->bmp_decode_us;
    dst->bmp_total_us = src->bmp_total_us;
    dst->bmp_max_us = src->bmp_max_us;
    dst->texture_calls = src->texture_calls;
    dst->texture_bytes = src->texture_bytes;
    dst->texture_us = src->texture_us;
    dst->rendercopy_calls = src->rendercopy_calls;
    dst->rendercopy_us = src->rendercopy_us;
    dst->rendercopy_direct_calls = src->rendercopy_direct_calls;
    dst->rendercopy_gpu2d_calls = src->rendercopy_gpu2d_calls;
    dst->rendercopy_gpu2d_miss = src->rendercopy_gpu2d_miss;
    dst->rendercopy_native_calls = src->rendercopy_native_calls;
    dst->rendercopy_blitbuf_calls = src->rendercopy_blitbuf_calls;
    dst->rendercopy_row_calls = src->rendercopy_row_calls;
    dst->rendercopy_fill_calls = src->rendercopy_fill_calls;
    dst->rendercopy_soft_calls = src->rendercopy_soft_calls;
    dst->rendercopy_pixels = src->rendercopy_pixels;
    dst->fill_calls = src->fill_calls;
    dst->fill_flushes = src->fill_flushes;
    dst->fill_pixels = src->fill_pixels;
    dst->fill_us = src->fill_us;
    dst->clear_calls = src->clear_calls;
    dst->clear_us = src->clear_us;
    dst->poll_calls = src->poll_calls;
    dst->poll_us = src->poll_us;
    dst->present_calls = src->present_calls;
    dst->present_us = src->present_us;
    dst->present_flush_us = src->present_flush_us;
    dst->present_upload_us = src->present_upload_us;
    dst->present_kernel_us = src->present_kernel_us;
}

void SDL_QOS_ProfileReset(void){
    g_sdl_profile.bmp_calls = 0ull;
    g_sdl_profile.bmp_ok = 0ull;
    g_sdl_profile.bmp_fail = 0ull;
    g_sdl_profile.bmp_bytes = 0ull;
    g_sdl_profile.bmp_read_us = 0ull;
    g_sdl_profile.bmp_decode_us = 0ull;
    g_sdl_profile.bmp_total_us = 0ull;
    g_sdl_profile.bmp_max_us = 0ull;
    g_sdl_profile.texture_calls = 0ull;
    g_sdl_profile.texture_bytes = 0ull;
    g_sdl_profile.texture_us = 0ull;
    g_sdl_profile.rendercopy_calls = 0ull;
    g_sdl_profile.rendercopy_us = 0ull;
    g_sdl_profile.rendercopy_direct_calls = 0ull;
    g_sdl_profile.rendercopy_gpu2d_calls = 0ull;
    g_sdl_profile.rendercopy_gpu2d_miss = 0ull;
    g_sdl_profile.rendercopy_native_calls = 0ull;
    g_sdl_profile.rendercopy_blitbuf_calls = 0ull;
    g_sdl_profile.rendercopy_row_calls = 0ull;
    g_sdl_profile.rendercopy_fill_calls = 0ull;
    g_sdl_profile.rendercopy_soft_calls = 0ull;
    g_sdl_profile.rendercopy_pixels = 0ull;
    g_sdl_profile.fill_calls = 0ull;
    g_sdl_profile.fill_flushes = 0ull;
    g_sdl_profile.fill_pixels = 0ull;
    g_sdl_profile.fill_us = 0ull;
    g_sdl_profile.clear_calls = 0ull;
    g_sdl_profile.clear_us = 0ull;
    g_sdl_profile.poll_calls = 0ull;
    g_sdl_profile.poll_us = 0ull;
    g_sdl_profile.present_calls = 0ull;
    g_sdl_profile.present_us = 0ull;
    g_sdl_profile.present_flush_us = 0ull;
    g_sdl_profile.present_upload_us = 0ull;
    g_sdl_profile.present_kernel_us = 0ull;
    sdl_profile_copy(&g_sdl_auto_last_profile, &g_sdl_profile);
    g_sdl_auto_last_us = qos_get_time_us();
    qos_file_profile_reset();
}

void SDL_QOS_ProfileDump(void){
    qos_puts("SDL profile: bmp calls=");
    sdl_profile_put_u64(g_sdl_profile.bmp_calls);
    qos_puts(" ok=");
    sdl_profile_put_u64(g_sdl_profile.bmp_ok);
    qos_puts(" fail=");
    sdl_profile_put_u64(g_sdl_profile.bmp_fail);
    qos_puts(" bytes=");
    sdl_profile_put_u64(g_sdl_profile.bmp_bytes);
    qos_puts("\n");

    qos_puts("SDL profile us: bmp_read=");
    sdl_profile_put_u64(g_sdl_profile.bmp_read_us);
    qos_puts(" bmp_decode=");
    sdl_profile_put_u64(g_sdl_profile.bmp_decode_us);
    qos_puts(" bmp_total=");
    sdl_profile_put_u64(g_sdl_profile.bmp_total_us);
    qos_puts(" bmp_max=");
    sdl_profile_put_u64(g_sdl_profile.bmp_max_us);
    qos_puts("\n");

    qos_puts("SDL profile us: texture calls=");
    sdl_profile_put_u64(g_sdl_profile.texture_calls);
    qos_puts(" bytes=");
    sdl_profile_put_u64(g_sdl_profile.texture_bytes);
    qos_puts(" total=");
    sdl_profile_put_u64(g_sdl_profile.texture_us);
    qos_puts("\n");

    qos_puts("SDL profile us: rendercopy calls=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_calls);
    qos_puts(" total=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_us);
    qos_puts(" pixels=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_pixels);
    qos_puts("\n");

    qos_puts("SDL render paths: direct=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_direct_calls);
    qos_puts(" gpu2d=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_gpu2d_calls);
    qos_puts(" gpu2d_miss=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_gpu2d_miss);
    qos_puts(" native=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_native_calls);
    qos_puts(" blitbuf=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_blitbuf_calls);
    qos_puts(" row=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_row_calls);
    qos_puts(" fill=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_fill_calls);
    qos_puts(" soft=");
    sdl_profile_put_u64(g_sdl_profile.rendercopy_soft_calls);
    qos_puts("\n");

    qos_puts("SDL profile us: clear calls=");
    sdl_profile_put_u64(g_sdl_profile.clear_calls);
    qos_puts(" total=");
    sdl_profile_put_u64(g_sdl_profile.clear_us);
    qos_puts(" fill calls=");
    sdl_profile_put_u64(g_sdl_profile.fill_calls);
    qos_puts(" flushes=");
    sdl_profile_put_u64(g_sdl_profile.fill_flushes);
    qos_puts(" pixels=");
    sdl_profile_put_u64(g_sdl_profile.fill_pixels);
    qos_puts(" total=");
    sdl_profile_put_u64(g_sdl_profile.fill_us);
    qos_puts("\n");

    qos_puts("SDL profile us: poll calls=");
    sdl_profile_put_u64(g_sdl_profile.poll_calls);
    qos_puts(" total=");
    sdl_profile_put_u64(g_sdl_profile.poll_us);
    qos_puts(" present calls=");
    sdl_profile_put_u64(g_sdl_profile.present_calls);
    qos_puts(" total=");
    sdl_profile_put_u64(g_sdl_profile.present_us);
    qos_puts("\n");

    qos_puts("SDL profile us: present flush=");
    sdl_profile_put_u64(g_sdl_profile.present_flush_us);
    qos_puts(" upload=");
    sdl_profile_put_u64(g_sdl_profile.present_upload_us);
    qos_puts(" kernel=");
    sdl_profile_put_u64(g_sdl_profile.present_kernel_us);
    qos_puts("\n");

    qos_puts("SDL heap: used=");
    sdl_profile_put_u64((Uint64)qos_heap_used());
    qos_puts(" free=");
    sdl_profile_put_u64((Uint64)qos_heap_free());
    qos_puts(" largest=");
    sdl_profile_put_u64((Uint64)qos_heap_largest_free());
    qos_puts(" total=");
    sdl_profile_put_u64((Uint64)qos_heap_total());
    qos_puts("\n");

    qos_file_profile_dump();
    qos_puts("SDL profile: file profile complete\n");
}

#ifdef QOS_PROFILE_SDL_AUTO
static void sdl_profile_auto_tick(void){
    Uint64 now_us = qos_get_time_us();
    if (g_sdl_auto_last_us != 0ull &&
        now_us - g_sdl_auto_last_us < 1000000ull){
        return;
    }

    Uint64 frames = sdl_profile_delta(g_sdl_profile.present_calls,
                                      g_sdl_auto_last_profile.present_calls);
    Uint64 window_us = (now_us >= g_sdl_auto_last_us) ?
                       (now_us - g_sdl_auto_last_us) : 0ull;
    if (frames == 0ull){
        g_sdl_auto_last_us = now_us;
        sdl_profile_copy(&g_sdl_auto_last_profile, &g_sdl_profile);
        return;
    }

    Uint64 clear_calls = sdl_profile_delta(g_sdl_profile.clear_calls,
                                           g_sdl_auto_last_profile.clear_calls);
    Uint64 poll_calls = sdl_profile_delta(g_sdl_profile.poll_calls,
                                          g_sdl_auto_last_profile.poll_calls);
    Uint64 render_calls = sdl_profile_delta(g_sdl_profile.rendercopy_calls,
                                            g_sdl_auto_last_profile.rendercopy_calls);
    Uint64 fill_calls = sdl_profile_delta(g_sdl_profile.fill_calls,
                                          g_sdl_auto_last_profile.fill_calls);
    Uint64 fill_pixels = sdl_profile_delta(g_sdl_profile.fill_pixels,
                                           g_sdl_auto_last_profile.fill_pixels);
    Uint64 clear_us = sdl_profile_delta(g_sdl_profile.clear_us,
                                        g_sdl_auto_last_profile.clear_us);
    Uint64 poll_us = sdl_profile_delta(g_sdl_profile.poll_us,
                                       g_sdl_auto_last_profile.poll_us);
    Uint64 render_us = sdl_profile_delta(g_sdl_profile.rendercopy_us,
                                         g_sdl_auto_last_profile.rendercopy_us);
    Uint64 present_us = sdl_profile_delta(g_sdl_profile.present_us,
                                          g_sdl_auto_last_profile.present_us);
    Uint64 present_flush_us = sdl_profile_delta(g_sdl_profile.present_flush_us,
                                                g_sdl_auto_last_profile.present_flush_us);
    Uint64 present_upload_us = sdl_profile_delta(g_sdl_profile.present_upload_us,
                                                 g_sdl_auto_last_profile.present_upload_us);
    Uint64 present_kernel_us = sdl_profile_delta(g_sdl_profile.present_kernel_us,
                                                 g_sdl_auto_last_profile.present_kernel_us);
    Uint64 direct = sdl_profile_delta(g_sdl_profile.rendercopy_direct_calls,
                                      g_sdl_auto_last_profile.rendercopy_direct_calls);
    Uint64 gpu2d = sdl_profile_delta(g_sdl_profile.rendercopy_gpu2d_calls,
                                     g_sdl_auto_last_profile.rendercopy_gpu2d_calls);
    Uint64 native = sdl_profile_delta(g_sdl_profile.rendercopy_native_calls,
                                      g_sdl_auto_last_profile.rendercopy_native_calls);
    Uint64 blitbuf = sdl_profile_delta(g_sdl_profile.rendercopy_blitbuf_calls,
                                       g_sdl_auto_last_profile.rendercopy_blitbuf_calls);
    Uint64 row = sdl_profile_delta(g_sdl_profile.rendercopy_row_calls,
                                   g_sdl_auto_last_profile.rendercopy_row_calls);
    Uint64 fill = sdl_profile_delta(g_sdl_profile.rendercopy_fill_calls,
                                    g_sdl_auto_last_profile.rendercopy_fill_calls);
    Uint64 soft = sdl_profile_delta(g_sdl_profile.rendercopy_soft_calls,
                                    g_sdl_auto_last_profile.rendercopy_soft_calls);
    Uint64 pixels = sdl_profile_delta(g_sdl_profile.rendercopy_pixels,
                                      g_sdl_auto_last_profile.rendercopy_pixels);

    qos_puts("SDL 1s: frames=");
    sdl_profile_put_u64(frames);
    qos_puts(" win_ms=");
    sdl_profile_put_u64(window_us / 1000ull);
    qos_puts(" avg_us clear=");
    sdl_profile_put_u64(clear_us / frames);
    qos_puts(" poll=");
    sdl_profile_put_u64(poll_us / frames);
    qos_puts(" copy=");
    sdl_profile_put_u64(render_us / frames);
    qos_puts(" present=");
    sdl_profile_put_u64(present_us / frames);
    qos_puts(" up=");
    sdl_profile_put_u64(present_upload_us / frames);
    qos_puts(" kern=");
    sdl_profile_put_u64(present_kernel_us / frames);
    qos_puts(" flush=");
    sdl_profile_put_u64(present_flush_us / frames);
    qos_puts(" direct=");
    sdl_profile_put_u64((Uint64)(g_soft_fb_direct ? 1u : 0u));
    qos_puts(" calls c/p/r/f=");
    sdl_profile_put_u64(clear_calls / frames);
    qos_putc('/');
    sdl_profile_put_u64(poll_calls / frames);
    qos_putc('/');
    sdl_profile_put_u64(render_calls / frames);
    qos_putc('/');
    sdl_profile_put_u64(fill_calls / frames);
    qos_puts(" paths d/g/n/b/r/f/s=");
    sdl_profile_put_u64(direct);
    qos_putc('/');
    sdl_profile_put_u64(gpu2d);
    qos_putc('/');
    sdl_profile_put_u64(native);
    qos_putc('/');
    sdl_profile_put_u64(blitbuf);
    qos_putc('/');
    sdl_profile_put_u64(row);
    qos_putc('/');
    sdl_profile_put_u64(fill);
    qos_putc('/');
    sdl_profile_put_u64(soft);
    qos_puts(" px=");
    sdl_profile_put_u64(pixels);
    qos_puts(" pxpf=");
    sdl_profile_put_u64(pixels / frames);
    qos_puts(" fillpxpf=");
    sdl_profile_put_u64(fill_pixels / frames);
    qos_puts("\n");

    g_sdl_auto_last_us = now_us;
    sdl_profile_copy(&g_sdl_auto_last_profile, &g_sdl_profile);
}
#endif

static Uint32 rgb_to_color(Uint8 r, Uint8 g, Uint8 b){
    return ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

static Uint32 rgba_to_color(Uint8 r, Uint8 g, Uint8 b, Uint8 a){
    return ((Uint32)r << 24) | ((Uint32)g << 16) | ((Uint32)b << 8) | (Uint32)a;
}

static Uint8 color_r(Uint32 c){
    return (Uint8)((c >> 24) & 0xFFu);
}

static Uint8 color_g(Uint32 c){
    return (Uint8)((c >> 16) & 0xFFu);
}

static Uint8 color_b(Uint32 c){
    return (Uint8)((c >> 8) & 0xFFu);
}

static Uint8 color_a(Uint32 c){
    return (Uint8)(c & 0xFFu);
}

static void sdl_soft_backbuffer_destroy(void){
    if (g_soft_fb && !g_soft_fb_direct){
        free(g_soft_fb);
    }
    g_soft_fb = 0;
    g_soft_fb_w = 0;
    g_soft_fb_h = 0;
    g_soft_fb_pitch = 0;
    g_soft_fb_enabled = 0;
    g_soft_fb_dirty = 0;
    g_soft_fb_attached = 0;
    g_soft_fb_direct = 0;
    g_soft_fb_direct_inactive = 0;
    g_soft_fb_direct_checked = 0;
}

static int sdl_direct_drop_frame(void){
    if (!g_soft_fb_direct){
        return -1;
    }
    g_soft_fb = 0;
    g_soft_fb_pitch = 0;
    g_soft_fb_direct_inactive = 1;
    return 0;
}

static int sdl_direct_refresh_for_draw(void){
    qos_fb_direct_info_t info;
    int rc;
    if (!g_soft_fb_direct){
        return 1;
    }
    rc = qos_fb_direct_get_draw(&info);
    if (rc == 0 &&
        info.pixels &&
        info.width == (unsigned int)g_soft_fb_w &&
        info.height == (unsigned int)g_soft_fb_h &&
        info.pitch >= ((unsigned int)g_soft_fb_w * 4u)){
        g_soft_fb = (Uint8*)info.pixels;
        g_soft_fb_pitch = (int)info.pitch;
        g_soft_fb_direct_inactive = 0;
        return 1;
    }
    if (rc == -2){
        (void)sdl_direct_drop_frame();
        return 0;
    }
    return -1;
}

static int sdl_direct_refresh_frame(void){
    if (!g_soft_fb_direct){
        return 1;
    }
    if (g_soft_fb_direct_checked){
        return g_soft_fb_direct_inactive ? 0 : 1;
    }
    g_soft_fb_direct_checked = 1;
    return sdl_direct_refresh_for_draw();
}

static int sdl_soft_backbuffer_init(int w, int h){
#if SDL_SHIM_SOFT_BACKBUFFER
    unsigned long bytes;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096){
        return -1;
    }
    if (g_soft_fb && g_soft_fb_w == w && g_soft_fb_h == h){
        g_soft_fb_enabled = 1;
        if (!g_soft_fb_direct &&
            !g_soft_fb_attached &&
            qos_fb_attach_buffer((const unsigned int*)g_soft_fb,
                                 (unsigned int)w,
                                 (unsigned int)h,
                                 (unsigned int)g_soft_fb_pitch) == 0){
            g_soft_fb_attached = 1;
        }
        return 0;
    }
    sdl_soft_backbuffer_destroy();

    qos_fb_direct_info_t direct;
    int direct_rc = qos_fb_direct_acquire(&direct);
    if (direct_rc == 0 &&
        direct.pixels &&
        direct.width == (unsigned int)w &&
        direct.height == (unsigned int)h &&
        direct.pitch >= ((unsigned int)w * 4u)){
        g_soft_fb = (Uint8*)direct.pixels;
        g_soft_fb_w = w;
        g_soft_fb_h = h;
        g_soft_fb_pitch = (int)direct.pitch;
        g_soft_fb_enabled = 1;
        g_soft_fb_dirty = 1;
        g_soft_fb_attached = 1;
        g_soft_fb_direct = 1;
        g_soft_fb_direct_inactive = 0;
        g_soft_fb_direct_checked = 0;
        return 0;
    }
    if (direct_rc < 0){
        qos_puts("SDL direct framebuffer unavailable rc=");
        sdl_profile_put_u64((Uint64)(unsigned int)(-direct_rc));
        qos_puts("; using buffered framebuffer\n");
    }

    bytes = (unsigned long)w * (unsigned long)h * 4ul;
    if (bytes > 64ul * 1024ul * 1024ul){
        return -1;
    }
    g_soft_fb = (Uint8*)malloc(bytes);
    if (!g_soft_fb){
        return -1;
    }
    g_soft_fb_w = w;
    g_soft_fb_h = h;
    g_soft_fb_pitch = w * 4;
    g_soft_fb_enabled = 1;
    g_soft_fb_dirty = 1;
    for (unsigned long i = 0; i < bytes; i++){
        g_soft_fb[i] = 0u;
    }
    if (qos_fb_attach_buffer((const unsigned int*)g_soft_fb,
                             (unsigned int)w,
                             (unsigned int)h,
                             (unsigned int)g_soft_fb_pitch) == 0){
        g_soft_fb_attached = 1;
    }
    return 0;
#else
    (void)w;
    (void)h;
    return -1;
#endif
}

static int sdl_soft_backbuffer_valid(const SDL_Renderer* renderer){
    return renderer &&
           renderer->alive &&
           renderer->window &&
           renderer->window->alive &&
           g_soft_fb_enabled &&
           g_soft_fb &&
           g_soft_fb_w == renderer->window->w &&
           g_soft_fb_h == renderer->window->h &&
           g_soft_fb_pitch == renderer->window->w * 4;
}

static void sdl_soft_fill_rect(unsigned int x,
                               unsigned int y,
                               unsigned int w,
                               unsigned int h,
                               unsigned int color){
    if (!g_soft_fb || w == 0u || h == 0u){
        return;
    }
    if (x >= (unsigned int)g_soft_fb_w || y >= (unsigned int)g_soft_fb_h){
        return;
    }
    if (x + w < x || x + w > (unsigned int)g_soft_fb_w){
        w = (unsigned int)g_soft_fb_w - x;
    }
    if (y + h < y || y + h > (unsigned int)g_soft_fb_h){
        h = (unsigned int)g_soft_fb_h - y;
    }
    Uint8 r = (Uint8)((color >> 16) & 0xFFu);
    Uint8 g = (Uint8)((color >> 8) & 0xFFu);
    Uint8 b = (Uint8)(color & 0xFFu);
    Uint32 packed = ((Uint32)r << 16) |
                    ((Uint32)g << 8) |
                    (Uint32)b;
    for (unsigned int py = 0u; py < h; py++){
        Uint32* row = (Uint32*)(g_soft_fb +
                                ((unsigned long)(y + py) * (unsigned long)g_soft_fb_pitch) +
                                ((unsigned long)x * 4ul));
        for (unsigned int px = 0u; px < w; px++){
            row[px] = packed;
        }
    }
    g_soft_fb_dirty = 1;
}

static void surface_set_format(SDL_Surface* surface, Uint32 format){
    if (!surface){
        return;
    }
    surface->format_storage.format = format;
    surface->format_storage.bytes_per_pixel = 4u;
    surface->format = &surface->format_storage;
}

static Uint32 surface_format_value(const SDL_Surface* surface){
    if (!surface || !surface->format){
        return 0u;
    }
    return surface->format->format;
}

static void set_error(const char* msg){
    int i = 0;
    if (!msg){
        g_last_error[0] = 0;
        return;
    }
    while (msg[i] && i < (int)(sizeof(g_last_error) - 1u)){
        g_last_error[i] = msg[i];
        i++;
    }
    g_last_error[i] = 0;
}

static void sdl_flush_pending_fill(void){
    if (!g_pending_fill_valid){
        return;
    }
    Uint64 t0 = qos_get_time_us();
    if (g_soft_fb_enabled && g_soft_fb){
        sdl_soft_fill_rect(g_pending_fill_x,
                           g_pending_fill_y,
                           g_pending_fill_w,
                           g_pending_fill_h,
                           g_pending_fill_color);
        g_sdl_profile.fill_flushes++;
        g_sdl_profile.fill_pixels += (Uint64)g_pending_fill_w * (Uint64)g_pending_fill_h;
        g_sdl_profile.fill_us += qos_get_time_us() - t0;
        g_pending_fill_valid = 0;
        return;
    }
    qos_fb_rect(g_pending_fill_x,
                g_pending_fill_y,
                g_pending_fill_w,
                g_pending_fill_h,
                g_pending_fill_color);
    g_sdl_profile.fill_flushes++;
    g_sdl_profile.fill_pixels += (Uint64)g_pending_fill_w * (Uint64)g_pending_fill_h;
    g_sdl_profile.fill_us += qos_get_time_us() - t0;
    g_pending_fill_valid = 0;
}

static int sdl_queue_fill_rect(unsigned int x,
                               unsigned int y,
                               unsigned int w,
                               unsigned int h,
                               unsigned int color){
    if (w == 0u || h == 0u){
        return 0;
    }
    g_sdl_profile.fill_calls++;
    Uint64 t0 = qos_get_time_us();
    if (g_soft_fb_direct &&
        (x & 63u) == 0u &&
        (y & 63u) == 0u &&
        (w & 63u) == 0u &&
        (h & 63u) == 0u &&
        qos_gpu2d_fill_rect(x, y, w, h, color) == 0){
        g_sdl_profile.fill_flushes++;
        g_sdl_profile.fill_pixels += (Uint64)w * (Uint64)h;
        g_sdl_profile.fill_us += qos_get_time_us() - t0;
        return 0;
    }
    if (g_soft_fb_enabled && g_soft_fb){
        sdl_soft_fill_rect(x, y, w, h, color);
        g_sdl_profile.fill_flushes++;
        g_sdl_profile.fill_pixels += (Uint64)w * (Uint64)h;
        g_sdl_profile.fill_us += qos_get_time_us() - t0;
        return 0;
    }
    if (g_pending_fill_valid &&
        g_pending_fill_y == y &&
        g_pending_fill_h == h &&
        g_pending_fill_color == color &&
        g_pending_fill_x + g_pending_fill_w == x){
        g_pending_fill_w += w;
        return 0;
    }

    sdl_flush_pending_fill();
    g_pending_fill_valid = 1;
    g_pending_fill_x = x;
    g_pending_fill_y = y;
    g_pending_fill_w = w;
    g_pending_fill_h = h;
    g_pending_fill_color = color;
    return 0;
}

static unsigned int sdl_gpu2d_status_cached(void){
    static int cached = 0;
    static unsigned int status = 0u;
    if (!cached){
        status = qos_gpu2d_status();
        cached = 1;
    }
    return status;
}

static int sdl_gpu2d_try_blit(SDL_Texture* texture,
                              int sx,
                              int sy,
                              int sw,
                              int sh,
                              int dx,
                              int dy,
                              int dw,
                              int dh,
                              int visible_x0,
                              int visible_y0,
                              int visible_x1,
                              int visible_y1,
                              int flip){
    qos_gpu2d_blit_t blit;
    unsigned int status = sdl_gpu2d_status_cached();

    if ((status & QOS_GPU2D_CAP_ACCEL_BLIT) == 0u){
        return -2;
    }
    if (!texture || !texture->pixels || texture->w <= 0 || texture->h <= 0 ||
        texture->pitch <= 0 || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0){
        return -1;
    }

    /*
     * Direct page-flip rendering writes straight into the mapped scanout page.
     * The first GPU2D ABI targets the process display session instead, so do
     * not mix the two until the hardware backend can target direct pages too.
     */
    if (g_soft_fb_direct){
        return -2;
    }

    if (visible_x0 != 0 || visible_y0 != 0 || visible_x1 != dw || visible_y1 != dh){
        return -2;
    }
    if ((sw != dw || sh != dh) && (status & QOS_GPU2D_CAP_ACCEL_SCALE) == 0u){
        return -2;
    }
    if ((flip & SDL_FLIP_HORIZONTAL) && (status & QOS_GPU2D_CAP_ACCEL_BLIT) == 0u){
        return -2;
    }
    if ((flip & SDL_FLIP_VERTICAL) && (status & QOS_GPU2D_CAP_ACCEL_BLIT) == 0u){
        return -2;
    }
    if ((texture->alpha_mod != 255u ||
         texture->color_r != 255u ||
         texture->color_g != 255u ||
         texture->color_b != 255u ||
         (!texture->opaque && texture->blend_mode != SDL_BLENDMODE_NONE)) &&
        (status & QOS_GPU2D_CAP_ACCEL_ALPHA) == 0u){
        return -2;
    }

    blit.pixels = texture->pixels;
    blit.texture_w = (unsigned int)texture->w;
    blit.texture_h = (unsigned int)texture->h;
    blit.pitch = (unsigned int)texture->pitch;
    blit.src_x = (unsigned int)sx;
    blit.src_y = (unsigned int)sy;
    blit.src_w = (unsigned int)sw;
    blit.src_h = (unsigned int)sh;
    blit.dst_x = dx;
    blit.dst_y = dy;
    blit.dst_w = (unsigned int)dw;
    blit.dst_h = (unsigned int)dh;
    blit.color_rgba = ((Uint32)texture->color_r << 24) |
                      ((Uint32)texture->color_g << 16) |
                      ((Uint32)texture->color_b << 8) |
                      (Uint32)texture->alpha_mod;
    blit.flags = 0u;
    if (texture->opaque){
        blit.flags |= QOS_GPU2D_BLIT_OPAQUE;
    }
    if (texture->blend_mode != SDL_BLENDMODE_NONE){
        blit.flags |= QOS_GPU2D_BLIT_BLEND;
    }
    if (flip & SDL_FLIP_HORIZONTAL){
        blit.flags |= QOS_GPU2D_BLIT_FLIP_X;
    }
    if (flip & SDL_FLIP_VERTICAL){
        blit.flags |= QOS_GPU2D_BLIT_FLIP_Y;
    }

    sdl_flush_pending_fill();
    return qos_gpu2d_blit_rgba(&blit);
}

static int sdl_i_min(int a, int b){
    return (a < b) ? a : b;
}

static int sdl_time_reached(Uint32 now, Uint32 target){
    return (int)(now - target) >= 0;
}

static void sdl_copy_bytes(Uint8* dst, const Uint8* src, Uint32 len);
static void sdl_zero_bytes(Uint8* dst, Uint32 len);
static void sdl_texture_free_gpu(SDL_Texture* texture);
static void sdl_texture_invalidate_gpu(SDL_Texture* texture);
static int sdl_texture_upload_gpu(SDL_Texture* texture);
static void sdl_texture_free_native(SDL_Texture* texture);
static void sdl_texture_invalidate_native(SDL_Texture* texture);
static int sdl_texture_ensure_native(SDL_Texture* texture);

static int sdl_soft_blit_native_texture(SDL_Texture* texture,
                                        int sx,
                                        int sy,
                                        int sw,
                                        int sh,
                                        int dx,
                                        int dy,
                                        int dw,
                                        int dh,
                                        int visible_x0,
                                        int visible_y0,
                                        int visible_x1,
                                        int visible_y1,
                                        int flip){
    if (!texture || !texture->native_pixels || !g_soft_fb ||
        sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        visible_x1 <= visible_x0 || visible_y1 <= visible_y0){
        return -1;
    }

    if (flip == SDL_FLIP_NONE &&
        visible_x0 == 0 && visible_y0 == 0 &&
        visible_x1 == dw && visible_y1 == dh &&
        sx == 0 && sy == 0 &&
        sw == texture->w && sh == texture->h &&
        dw == texture->w && dh == texture->h){
        for (int y = 0; y < dh; y++){
            Uint8* dst = g_soft_fb +
                         ((unsigned long)(dy + y) * (unsigned long)g_soft_fb_pitch) +
                         ((unsigned long)dx * 4ul);
            const Uint8* src = (const Uint8*)(texture->native_pixels +
                                              ((unsigned long)y * (unsigned long)texture->w));
            sdl_copy_bytes(dst, src, (Uint32)dw * 4u);
        }
        return 0;
    }

    unsigned long long x_step = ((unsigned long long)(unsigned int)sw << 32) /
                                (unsigned int)dw;
    unsigned long long y_step = ((unsigned long long)(unsigned int)sh << 32) /
                                (unsigned int)dh;
    long long y_acc = (long long)(((flip & SDL_FLIP_VERTICAL) ?
                                  (dh - 1 - visible_y0) :
                                  visible_y0) * y_step);
    long long y_delta = (flip & SDL_FLIP_VERTICAL) ?
                        -(long long)y_step :
                        (long long)y_step;

    for (int oy = visible_y0; oy < visible_y1; oy++){
        int py = dy + oy;
        if (py < 0 || py >= g_soft_fb_h){
            y_acc += y_delta;
            continue;
        }

        int tx_y = sy + (int)(((unsigned long long)y_acc) >> 32);
        if (tx_y < sy){
            tx_y = sy;
        } else if (tx_y >= sy + sh){
            tx_y = sy + sh - 1;
        }

        const Uint32* src_row = texture->native_pixels +
                                ((Uint32)tx_y * (Uint32)texture->w);
        Uint32* dst = (Uint32*)(g_soft_fb +
                                ((unsigned long)py * (unsigned long)g_soft_fb_pitch) +
                                ((unsigned long)(dx + visible_x0) * 4ul));
        long long x_acc = (long long)(((flip & SDL_FLIP_HORIZONTAL) ?
                                      (dw - 1 - visible_x0) :
                                      visible_x0) * x_step);
        long long x_delta = (flip & SDL_FLIP_HORIZONTAL) ?
                            -(long long)x_step :
                            (long long)x_step;

        for (int ox = visible_x0; ox < visible_x1; ox++){
            int tx_x = sx + (int)(((unsigned long long)x_acc) >> 32);
            if (tx_x < sx){
                tx_x = sx;
            } else if (tx_x >= sx + sw){
                tx_x = sx + sw - 1;
            }
            *dst++ = src_row[tx_x];
            x_acc += x_delta;
        }
        y_acc += y_delta;
    }
    return 0;
}

static int sdl_soft_blit_rgba_unscaled_texture(SDL_Texture* texture,
                                               int sx,
                                               int sy,
                                               int sw,
                                               int sh,
                                               int dx,
                                               int dy,
                                               int dw,
                                               int dh,
                                               int visible_x0,
                                               int visible_y0,
                                               int visible_x1,
                                               int visible_y1,
                                               int flip){
    if (!texture || !texture->pixels || !g_soft_fb ||
        sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        sw != dw || sh != dh ||
        flip != SDL_FLIP_NONE ||
        texture->alpha_mod != 255u ||
        texture->color_r != 255u ||
        texture->color_g != 255u ||
        texture->color_b != 255u ||
        visible_x1 <= visible_x0 || visible_y1 <= visible_y0){
        return -2;
    }

    for (int oy = visible_y0; oy < visible_y1; oy++){
        int py = dy + oy;
        if (py < 0 || py >= g_soft_fb_h){
            continue;
        }

        const Uint8* src = texture->pixels +
                           ((unsigned long)(sy + oy) * (unsigned long)texture->pitch) +
                           ((unsigned long)(sx + visible_x0) * 4ul);
        Uint32* dst = (Uint32*)(g_soft_fb +
                                ((unsigned long)py * (unsigned long)g_soft_fb_pitch) +
                                ((unsigned long)(dx + visible_x0) * 4ul));

        if (texture->blend_mode == SDL_BLENDMODE_NONE){
            for (int ox = visible_x0; ox < visible_x1; ox++){
                (void)ox;
                *dst++ = ((Uint32)src[0] << 16) |
                         ((Uint32)src[1] << 8) |
                         (Uint32)src[2];
                src += 4u;
            }
            continue;
        }

        for (int ox = visible_x0; ox < visible_x1; ox++){
            (void)ox;
            Uint32 sa = src[3];
            if (sa == 0u){
                src += 4u;
                dst++;
                continue;
            }

            Uint32 sr = src[0];
            Uint32 sg = src[1];
            Uint32 sb = src[2];
            if (sa >= 255u){
                *dst = (sr << 16) | (sg << 8) | sb;
            } else{
                Uint32 dc = *dst;
                Uint32 ia = 255u - sa;
                Uint32 dr = (dc >> 16) & 0xFFu;
                Uint32 dg = (dc >> 8) & 0xFFu;
                Uint32 db = dc & 0xFFu;
                Uint32 orv = (sr * sa + dr * ia + 127u) / 255u;
                Uint32 ogv = (sg * sa + dg * ia + 127u) / 255u;
                Uint32 obv = (sb * sa + db * ia + 127u) / 255u;
                *dst = (orv << 16) | (ogv << 8) | obv;
            }
            src += 4u;
            dst++;
        }
    }

    return 0;
}

static int sdl_soft_blit_texture(SDL_Texture* texture,
                                 int sx,
                                 int sy,
                                 int sw,
                                 int sh,
                                 int dx,
                                 int dy,
                                 int dw,
                                 int dh,
                                 int visible_x0,
                                 int visible_y0,
                                 int visible_x1,
                                 int visible_y1,
                                 int flip){
    int native_ok = 0;
    if (!g_soft_fb || !texture || !texture->pixels || dw <= 0 || dh <= 0){
        return -1;
    }

    if (texture->opaque &&
        texture->alpha_mod == 255u &&
        texture->color_r == 255u &&
        texture->color_g == 255u &&
        texture->color_b == 255u &&
        sdl_texture_ensure_native(texture) == 0){
        native_ok = 1;
    }

    if (flip == SDL_FLIP_NONE &&
        visible_x0 == 0 && visible_y0 == 0 &&
        visible_x1 == dw && visible_y1 == dh &&
        sx == 0 && sy == 0 &&
        sw == texture->w && sh == texture->h &&
        dw == texture->w && dh == texture->h &&
        texture->alpha_mod == 255u &&
        texture->color_r == 255u &&
        texture->color_g == 255u &&
        texture->color_b == 255u &&
        native_ok){
        if (sdl_soft_blit_native_texture(texture,
                                         sx,
                                         sy,
                                         sw,
                                         sh,
                                         dx,
                                         dy,
                                         dw,
                                         dh,
                                         visible_x0,
                                         visible_y0,
                                         visible_x1,
                                         visible_y1,
                                         flip) != 0){
            return -1;
        }
        g_sdl_profile.rendercopy_native_calls++;
        g_soft_fb_dirty = 1;
        return 0;
    }

#if SDL_SHIM_ENABLE_NATIVE_SCALED_FASTPATH
    if (native_ok){
        if (sdl_soft_blit_native_texture(texture,
                                         sx,
                                         sy,
                                         sw,
                                         sh,
                                         dx,
                                         dy,
                                         dw,
                                         dh,
                                         visible_x0,
                                         visible_y0,
                                         visible_x1,
                                         visible_y1,
                                         flip) != 0){
            return -1;
        }
        g_sdl_profile.rendercopy_native_calls++;
        g_soft_fb_dirty = 1;
        return 0;
    }
#endif

    if (sdl_soft_blit_rgba_unscaled_texture(texture,
                                            sx,
                                            sy,
                                            sw,
                                            sh,
                                            dx,
                                            dy,
                                            dw,
                                            dh,
                                            visible_x0,
                                            visible_y0,
                                            visible_x1,
                                            visible_y1,
                                            flip) == 0){
        g_soft_fb_dirty = 1;
        return 0;
    }

    for (int oy = visible_y0; oy < visible_y1; oy++){
        int py = dy + oy;
        if (py < 0 || py >= g_soft_fb_h){
            continue;
        }
        for (int ox = visible_x0; ox < visible_x1; ox++){
            int px = dx + ox;
            if (px < 0 || px >= g_soft_fb_w){
                continue;
            }

            int src_ox = (flip & SDL_FLIP_HORIZONTAL) ? (dw - 1 - ox) : ox;
            int src_oy = (flip & SDL_FLIP_VERTICAL) ? (dh - 1 - oy) : oy;
            int tx_x = sx + (int)(((unsigned long long)src_ox * (unsigned long long)sw) /
                                  (unsigned long long)dw);
            int tx_y = sy + (int)(((unsigned long long)src_oy * (unsigned long long)sh) /
                                  (unsigned long long)dh);
            const Uint8* sp = texture->pixels +
                              ((unsigned long)tx_y * (unsigned long)texture->pitch) +
                              ((unsigned long)tx_x * 4ul);
            Uint32* dp = (Uint32*)(g_soft_fb +
                                   ((unsigned long)py * (unsigned long)g_soft_fb_pitch) +
                                   ((unsigned long)px * 4ul));

            Uint32 sr = ((Uint32)sp[0] * (Uint32)texture->color_r) / 255u;
            Uint32 sg = ((Uint32)sp[1] * (Uint32)texture->color_g) / 255u;
            Uint32 sb = ((Uint32)sp[2] * (Uint32)texture->color_b) / 255u;
            Uint32 sa = (texture->blend_mode == SDL_BLENDMODE_NONE)
                            ? 255u
                            : (((Uint32)sp[3] * (Uint32)texture->alpha_mod) / 255u);

            if (sa == 0u){
                continue;
            }
            if (sa >= 255u){
                *dp = (sr << 16) | (sg << 8) | sb;
            } else{
                Uint32 ia = 255u - sa;
                Uint32 dc = *dp;
                Uint32 dr = (dc >> 16) & 0xFFu;
                Uint32 dg = (dc >> 8) & 0xFFu;
                Uint32 db = dc & 0xFFu;
                Uint32 orv = (sr * sa + dr * ia + 127u) / 255u;
                Uint32 ogv = (sg * sa + dg * ia + 127u) / 255u;
                Uint32 obv = (sb * sa + db * ia + 127u) / 255u;
                *dp = (orv << 16) | (ogv << 8) | obv;
            }
        }
    }
    g_soft_fb_dirty = 1;
    return 0;
}

static Uint32 sdl_now_ms(void){
    return (Uint32)(qos_get_time_us() / 1000ull);
}

static void sdl_event_clear(SDL_Event* event){
    Uint8* p = (Uint8*)event;
    if (!event){
        return;
    }
    for (Uint32 i = 0u; i < (Uint32)sizeof(SDL_Event); i++){
        p[i] = 0u;
    }
}

static int sdl_event_push(const SDL_Event* event){
    if (!event){
        return -1;
    }
    if (g_event_count >= SDL_SHIM_EVENT_QUEUE_SIZE){
        g_event_head++;
        if (g_event_head >= SDL_SHIM_EVENT_QUEUE_SIZE){
            g_event_head = 0;
        }
        g_event_count--;
    }
    g_event_queue[g_event_tail] = *event;
    g_event_tail++;
    if (g_event_tail >= SDL_SHIM_EVENT_QUEUE_SIZE){
        g_event_tail = 0;
    }
    g_event_count++;
    return 0;
}

static int sdl_event_pop(SDL_Event* event){
    if (!event || g_event_count <= 0){
        return 0;
    }
    *event = g_event_queue[g_event_head];
    g_event_head++;
    if (g_event_head >= SDL_SHIM_EVENT_QUEUE_SIZE){
        g_event_head = 0;
    }
    g_event_count--;
    return 1;
}

static void sdl_event_queue_reset(void){
    g_event_head = 0;
    g_event_tail = 0;
    g_event_count = 0;
}

static Uint16 sdl_mod_from_qos(unsigned int qos_mod){
    Uint16 mod = KMOD_NONE;
    if (qos_mod & QOS_KEYMOD_SHIFT){
        mod |= KMOD_SHIFT;
    }
    if (qos_mod & QOS_KEYMOD_CTRL){
        mod |= KMOD_CTRL;
    }
    if (qos_mod & QOS_KEYMOD_ALT){
        mod |= KMOD_ALT;
    }
    if (qos_mod & QOS_KEYMOD_META){
        mod |= KMOD_GUI;
    }
    return mod;
}

static SDL_Scancode sdl_scancode_from_ascii(unsigned int ascii){
    if (ascii >= 'a' && ascii <= 'z'){
        return (SDL_Scancode)(SDL_SCANCODE_A + (int)(ascii - 'a'));
    }
    if (ascii >= 'A' && ascii <= 'Z'){
        return (SDL_Scancode)(SDL_SCANCODE_A + (int)(ascii - 'A'));
    }
    if (ascii >= '1' && ascii <= '9'){
        return (SDL_Scancode)(SDL_SCANCODE_1 + (int)(ascii - '1'));
    }
    if (ascii == '0'){
        return SDL_SCANCODE_0;
    }
    if (ascii == '\r' || ascii == '\n'){
        return SDL_SCANCODE_RETURN;
    }
    if (ascii == '\b' || ascii == 127u){
        return SDL_SCANCODE_BACKSPACE;
    }
    if (ascii == '\t'){
        return SDL_SCANCODE_TAB;
    }
    if (ascii == ' '){
        return SDL_SCANCODE_SPACE;
    }
    if (ascii == 27u){
        return SDL_SCANCODE_ESCAPE;
    }
    return SDL_SCANCODE_UNKNOWN;
}

static SDL_Scancode sdl_scancode_from_qos(const qos_event_t* ev){
    if (!ev){
        return SDL_SCANCODE_UNKNOWN;
    }
    if (ev->keycode > 0u && ev->keycode < (unsigned int)SDL_NUM_SCANCODES){
        return (SDL_Scancode)ev->keycode;
    }
    return sdl_scancode_from_ascii(ev->ascii);
}

static SDL_Keycode sdl_key_sym_from_qos(const qos_event_t* ev){
    if (!ev){
        return 0;
    }
    if (ev->ascii != 0u){
        if (ev->ascii == '\n'){
            return SDLK_RETURN;
        }
        if (ev->ascii == 127u){
            return SDLK_BACKSPACE;
        }
        return (SDL_Keycode)ev->ascii;
    }
    if (ev->keycode >= SDL_SCANCODE_A && ev->keycode <= SDL_SCANCODE_Z){
        return (SDL_Keycode)('a' + (int)(ev->keycode - SDL_SCANCODE_A));
    }
    if (ev->keycode >= SDL_SCANCODE_1 && ev->keycode <= SDL_SCANCODE_9){
        return (SDL_Keycode)('1' + (int)(ev->keycode - SDL_SCANCODE_1));
    }
    if (ev->keycode == SDL_SCANCODE_0){
        return '0';
    }
    if (ev->keycode == SDL_SCANCODE_RETURN){
        return SDLK_RETURN;
    }
    if (ev->keycode == SDL_SCANCODE_ESCAPE){
        return SDLK_ESCAPE;
    }
    if (ev->keycode == SDL_SCANCODE_BACKSPACE){
        return SDLK_BACKSPACE;
    }
    if (ev->keycode == SDL_SCANCODE_TAB){
        return SDLK_TAB;
    }
    if (ev->keycode == SDL_SCANCODE_SPACE){
        return SDLK_SPACE;
    }
    if (ev->keycode == SDL_SCANCODE_RIGHT){
        return SDLK_RIGHT;
    }
    if (ev->keycode == SDL_SCANCODE_LEFT){
        return SDLK_LEFT;
    }
    if (ev->keycode == SDL_SCANCODE_DOWN){
        return SDLK_DOWN;
    }
    if (ev->keycode == SDL_SCANCODE_UP){
        return SDLK_UP;
    }
    if (ev->keycode == SDL_SCANCODE_LCTRL){
        return SDLK_LCTRL;
    }
    if (ev->keycode == SDL_SCANCODE_LSHIFT){
        return SDLK_LSHIFT;
    }
    if (ev->keycode == SDL_SCANCODE_LALT){
        return SDLK_LALT;
    }
    if (ev->keycode == SDL_SCANCODE_LGUI){
        return SDLK_LGUI;
    }
    if (ev->keycode == SDL_SCANCODE_RCTRL){
        return SDLK_RCTRL;
    }
    if (ev->keycode == SDL_SCANCODE_RSHIFT){
        return SDLK_RSHIFT;
    }
    if (ev->keycode == SDL_SCANCODE_RALT){
        return SDLK_RALT;
    }
    if (ev->keycode == SDL_SCANCODE_RGUI){
        return SDLK_RGUI;
    }
    return (SDL_Keycode)ev->keycode;
}

static void sdl_fill_key_event(SDL_Event* event,
                               Uint32 type,
                               SDL_Scancode scancode,
                               SDL_Keycode sym,
                               Uint16 mod,
                               Uint8 repeat){
    sdl_event_clear(event);
    event->type = type;
    event->key.type = type;
    event->key.timestamp = sdl_now_ms();
    event->key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
    event->key.repeat = repeat;
    event->key.keysym.scancode = scancode;
    event->key.keysym.sym = sym;
    event->key.keysym.mod = mod;
}

static Uint32 sdl_mouse_mask_from_qos(unsigned int qos_buttons){
    Uint32 mask = 0u;
    if (qos_buttons & QOS_MOUSE_LEFT){
        mask |= SDL_BUTTON_LMASK;
    }
    if (qos_buttons & QOS_MOUSE_MIDDLE){
        mask |= SDL_BUTTON_MMASK;
    }
    if (qos_buttons & QOS_MOUSE_RIGHT){
        mask |= SDL_BUTTON_RMASK;
    }
    return mask;
}

static Uint8 sdl_button_from_qos(unsigned int qos_button){
    if (qos_button & QOS_MOUSE_LEFT){
        return SDL_BUTTON_LEFT;
    }
    if (qos_button & QOS_MOUSE_MIDDLE){
        return SDL_BUTTON_MIDDLE;
    }
    if (qos_button & QOS_MOUSE_RIGHT){
        return SDL_BUTTON_RIGHT;
    }
    return 0u;
}

static int sdl_poll_repeat_event(SDL_Event* event){
    Uint32 now;
    if (!event || !g_key_repeat_enabled){
        return 0;
    }
    now = sdl_now_ms();
    for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++){
        if (g_keyboard_state[sc] &&
            g_key_repeat_next_ms[sc] != 0u &&
            sdl_time_reached(now, g_key_repeat_next_ms[sc])){
            SDL_Keycode sym = g_key_sym[sc];
            if (sym == 0){
                sym = (SDL_Keycode)sc;
            }
            sdl_fill_key_event(event,
                               SDL_KEYDOWN,
                               (SDL_Scancode)sc,
                               sym,
                               g_key_mod[sc],
                               1u);
            g_key_repeat_next_ms[sc] = now + g_key_repeat_interval_ms;
            return 1;
        }
    }
    return 0;
}

static void sdl_queue_text_input_if_printable(const qos_event_t* qos_ev){
    SDL_Event text_event;
    if (!qos_ev ||
        qos_ev->type != QOS_EVENT_KEY_DOWN ||
        qos_ev->ascii < 32u ||
        qos_ev->ascii >= 127u){
        return;
    }
    sdl_event_clear(&text_event);
    text_event.type = SDL_TEXTINPUT;
    text_event.text.type = SDL_TEXTINPUT;
    text_event.text.text[0] = (char)qos_ev->ascii;
    text_event.text.text[1] = 0;
    (void)sdl_event_push(&text_event);
}

static int sdl_translate_qos_event(const qos_event_t* qos_ev, SDL_Event* event){
    if (!qos_ev || !event){
        return 0;
    }

    if (qos_ev->type == QOS_EVENT_KEY_DOWN ||
        qos_ev->type == QOS_EVENT_KEY_UP){
        SDL_Scancode scancode = sdl_scancode_from_qos(qos_ev);
        SDL_Keycode sym = sdl_key_sym_from_qos(qos_ev);
        Uint16 mod = sdl_mod_from_qos(qos_ev->modifiers);
        Uint8 repeat = 0u;
        Uint32 now = sdl_now_ms();

        g_mod_state = mod;
        if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_NUM_SCANCODES){
            if (qos_ev->type == QOS_EVENT_KEY_DOWN){
                repeat = g_keyboard_state[scancode] ? 1u : 0u;
                g_keyboard_state[scancode] = 1u;
                if (!repeat){
                    g_key_down_ms[scancode] = now;
                    g_key_repeat_next_ms[scancode] = now + g_key_repeat_delay_ms;
                }
                g_key_sym[scancode] = sym;
                g_key_mod[scancode] = mod;
            } else {
                g_keyboard_state[scancode] = 0u;
                g_key_down_ms[scancode] = 0u;
                g_key_repeat_next_ms[scancode] = 0u;
                g_key_sym[scancode] = 0;
                g_key_mod[scancode] = KMOD_NONE;
            }
        }

        sdl_fill_key_event(event,
                           (qos_ev->type == QOS_EVENT_KEY_DOWN) ? SDL_KEYDOWN : SDL_KEYUP,
                           scancode,
                           sym,
                           mod,
                           repeat);

        return 1;
    }

    if (qos_ev->source == QOS_EVENT_SOURCE_MOUSE){
        g_mouse_x = qos_ev->x;
        g_mouse_y = qos_ev->y;
        if (qos_ev->type == QOS_EVENT_MOUSE_MOVE){
            g_mouse_buttons = sdl_mouse_mask_from_qos(qos_ev->buttons);
            g_mouse_rel_x += qos_ev->dx;
            g_mouse_rel_y += qos_ev->dy;
        }
    }

    if (qos_ev->type == QOS_EVENT_MOUSE_MOVE){
        sdl_event_clear(event);
        event->type = SDL_MOUSEMOTION;
        event->motion.type = SDL_MOUSEMOTION;
        event->motion.state = g_mouse_buttons;
        event->motion.x = qos_ev->x;
        event->motion.y = qos_ev->y;
        event->motion.xrel = qos_ev->dx;
        event->motion.yrel = qos_ev->dy;
        return 1;
    }

    if (qos_ev->type == QOS_EVENT_MOUSE_BUTTON_DOWN ||
        qos_ev->type == QOS_EVENT_MOUSE_BUTTON_UP){
        Uint8 button = sdl_button_from_qos(qos_ev->button);
        Uint32 mask = (button != 0u) ? SDL_BUTTON(button) : 0u;
        Uint32 reported = sdl_mouse_mask_from_qos(qos_ev->buttons);

        if (qos_ev->buttons != 0u ||
            qos_ev->type == QOS_EVENT_MOUSE_BUTTON_UP){
            g_mouse_buttons = reported;
        }
        if (qos_ev->type == QOS_EVENT_MOUSE_BUTTON_DOWN){
            g_mouse_buttons |= mask;
        } else {
            g_mouse_buttons &= ~mask;
        }

        sdl_event_clear(event);
        event->type = (qos_ev->type == QOS_EVENT_MOUSE_BUTTON_DOWN) ?
                      SDL_MOUSEBUTTONDOWN :
                      SDL_MOUSEBUTTONUP;
        event->button.type = event->type;
        event->button.button = button;
        event->button.state = (qos_ev->type == QOS_EVENT_MOUSE_BUTTON_DOWN) ?
                              SDL_PRESSED :
                              SDL_RELEASED;
        event->button.clicks = 1u;
        event->button.padding1 = 0u;
        event->button.x = qos_ev->x;
        event->button.y = qos_ev->y;
        return 1;
    }

    if (qos_ev->type == QOS_EVENT_MOUSE_WHEEL){
        g_mouse_wheel_y += qos_ev->wheel;
        sdl_event_clear(event);
        event->type = SDL_MOUSEWHEEL;
        event->wheel.type = SDL_MOUSEWHEEL;
        event->wheel.x = 0;
        event->wheel.y = qos_ev->wheel;
        event->wheel.mouse_x = qos_ev->x;
        event->wheel.mouse_y = qos_ev->y;
        return 1;
    }

    return 0;
}

typedef Uint32 sdl_alias_u32 __attribute__((__may_alias__));
typedef Uint64 sdl_alias_u64 __attribute__((__may_alias__));

static void sdl_copy_bytes(Uint8* dst, const Uint8* src, Uint32 len){
    if (!dst || !src){
        return;
    }
    while (len >= 8u &&
           (((unsigned long)dst | (unsigned long)src) & 7ul) == 0ul){
        *(sdl_alias_u64*)dst = *(const sdl_alias_u64*)src;
        dst += 8u;
        src += 8u;
        len -= 8u;
    }
    while (len >= 4u &&
           (((unsigned long)dst | (unsigned long)src) & 3ul) == 0ul){
        *(sdl_alias_u32*)dst = *(const sdl_alias_u32*)src;
        dst += 4u;
        src += 4u;
        len -= 4u;
    }
    while (len > 0u){
        *dst++ = *src++;
        len--;
    }
}

static void sdl_zero_bytes(Uint8* dst, Uint32 len){
    Uint32 i;
    if (!dst){
        return;
    }
    for (i = 0u; i < len; i++){
        dst[i] = 0u;
    }
}

static Uint8* sdl_alloc_pixels(Uint32 bytes){
    if (bytes == 0u){
        return 0;
    }
    return (Uint8*)malloc((unsigned long)bytes);
}

static void sdl_free_pixels(Uint8* pixels){
    if (pixels){
        free(pixels);
    }
}

static void sdl_texture_free_gpu(SDL_Texture* texture){
    if (!texture){
        return;
    }
#if SDL_SHIM_ENABLE_GPU2D_TEXTURE_UPLOAD
    if (texture->gpu_texture_valid && texture->gpu_texture_id != 0u){
        (void)qos_gpu2d_texture_free(texture->gpu_texture_id);
    }
#endif
    texture->gpu_texture_id = 0u;
    texture->gpu_texture_valid = 0;
}

static void sdl_texture_invalidate_gpu(SDL_Texture* texture){
    sdl_texture_free_gpu(texture);
}

static int sdl_texture_upload_gpu(SDL_Texture* texture){
#if SDL_SHIM_ENABLE_GPU2D_TEXTURE_UPLOAD
    qos_gpu2d_texture_upload_t req;
    unsigned int status;

    if (!texture || !texture->alive || !texture->pixels ||
        texture->w <= 0 || texture->h <= 0 || texture->pitch <= 0){
        return -1;
    }
    if (texture->gpu_texture_valid && texture->gpu_texture_id != 0u){
        return 0;
    }

    status = sdl_gpu2d_status_cached();
    /*
     * Texture objects are only useful once the kernel backend can actually
     * consume them for accelerated blits. Avoid duplicating every BMP into
     * kernel memory on the current clear/fill-only V3D path.
     */
    if ((status & (QOS_GPU2D_CAP_TEXTURE_OBJECTS | QOS_GPU2D_CAP_ACCEL_BLIT)) !=
        (QOS_GPU2D_CAP_TEXTURE_OBJECTS | QOS_GPU2D_CAP_ACCEL_BLIT)){
        return -1;
    }

    req.pixels = (const unsigned int*)texture->pixels;
    req.width = (unsigned int)texture->w;
    req.height = (unsigned int)texture->h;
    req.pitch = (unsigned int)texture->pitch;
    req.flags = texture->opaque ? QOS_GPU2D_TEXTURE_OPAQUE : 0u;
    req.texture_id = 0u;

    if (qos_gpu2d_texture_upload(&req) != 0 || req.texture_id == 0u){
        return -1;
    }
    texture->gpu_texture_id = req.texture_id;
    texture->gpu_texture_valid = 1;
    return 0;
#else
    (void)texture;
    return -1;
#endif
}

static void sdl_texture_free_native(SDL_Texture* texture){
    if (!texture){
        return;
    }
    if (texture->native_pixels){
        free(texture->native_pixels);
    }
    texture->native_pixels = 0;
    texture->native_capacity = 0u;
    texture->native_valid = 0;
}

static void sdl_texture_invalidate_native(SDL_Texture* texture){
    if (texture){
        texture->native_valid = 0;
        sdl_texture_invalidate_gpu(texture);
    }
}

static int sdl_texture_ensure_native(SDL_Texture* texture){
    Uint32 bytes;
    if (!texture || !texture->alive || !texture->pixels ||
        texture->w <= 0 || texture->h <= 0 || texture->pitch <= 0){
        return -1;
    }
    if ((Uint32)texture->w > (0xFFFFFFFFu / (Uint32)texture->h) / 4u){
        return -1;
    }
    bytes = (Uint32)texture->w * (Uint32)texture->h * 4u;
    if (!texture->native_pixels || texture->native_capacity < bytes){
        sdl_texture_free_native(texture);
        texture->native_pixels = (Uint32*)malloc((unsigned long)bytes);
        if (!texture->native_pixels){
            return -1;
        }
        texture->native_capacity = bytes;
    }
    if (!texture->native_valid){
        for (int y = 0; y < texture->h; y++){
            const Uint8* src = texture->pixels + ((Uint32)y * (Uint32)texture->pitch);
            Uint32* dst = texture->native_pixels + ((Uint32)y * (Uint32)texture->w);
            for (int x = 0; x < texture->w; x++){
                const Uint8* sp = src + ((Uint32)x * 4u);
                dst[x] = ((Uint32)sp[0] << 16) |
                         ((Uint32)sp[1] << 8) |
                         (Uint32)sp[2];
            }
        }
        texture->native_valid = 1;
    }
    return 0;
}

static SDL_Surface* alloc_surface_slot(void){
    for (int i = 0; i < SDL_SHIM_MAX_SURFACES; i++){
        if (!g_surfaces[i].alive){
            return &g_surfaces[i];
        }
    }
    return 0;
}

static SDL_Texture* alloc_texture_slot(void){
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        if (!g_textures[i].alive){
            return &g_textures[i];
        }
    }
    return 0;
}

static Uint16 read_le16(const Uint8* p){
    return (Uint16)((Uint16)p[0] | ((Uint16)p[1] << 8));
}

static Uint32 read_le32(const Uint8* p){
    return (Uint32)p[0] |
           ((Uint32)p[1] << 8) |
           ((Uint32)p[2] << 16) |
           ((Uint32)p[3] << 24);
}

static int read_s32_le(const Uint8* p){
    return (int)read_le32(p);
}

static char sdl_ascii_lower(char c){
    if (c >= 'A' && c <= 'Z'){
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static int sdl_path_has_tile_hint(const char* path){
    if (!path){
        return 0;
    }
    for (const char* p = path; p[0] && p[1] && p[2] && p[3] && p[4]; p++){
        if (sdl_ascii_lower(p[0]) == 't' &&
            sdl_ascii_lower(p[1]) == 'i' &&
            sdl_ascii_lower(p[2]) == 'l' &&
            sdl_ascii_lower(p[3]) == 'e' &&
            p[4] == '_'){
            return 1;
        }
    }
    return 0;
}

static int sdl_path_has_char_hint(const char* path){
    if (!path){
        return 0;
    }
    for (const char* p = path; p[0] && p[1] && p[2] && p[3] && p[4] && p[5] && p[6]; p++){
        if ((p[0] == '/' || p[0] == '\\') &&
            sdl_ascii_lower(p[1]) == 'c' &&
            sdl_ascii_lower(p[2]) == 'h' &&
            sdl_ascii_lower(p[3]) == 'a' &&
            sdl_ascii_lower(p[4]) == 'r' &&
            sdl_ascii_lower(p[5]) == 's' &&
            (p[6] == '/' || p[6] == '\\')){
            return 1;
        }
    }
    {
        const char prefix[] = "img/chars/";
        for (unsigned int i = 0u; prefix[i]; i++){
            char got = path[i];
            char want = prefix[i];
            if (!got){
                return 0;
            }
            if (want == '/'){
                if (got != '/' && got != '\\'){
                    return 0;
                }
            } else if (sdl_ascii_lower(got) != want){
                return 0;
            }
        }
    }
    return 1;
}

static int clamp_src_rect(const SDL_Texture* t, int* sx, int* sy, int* sw, int* sh){
    if (!t || !sx || !sy || !sw || !sh){
        return -1;
    }
    if (*sx < 0){
        *sw += *sx;
        *sx = 0;
    }
    if (*sy < 0){
        *sh += *sy;
        *sy = 0;
    }
    if (*sx >= t->w || *sy >= t->h || *sw <= 0 || *sh <= 0){
        return -1;
    }
    if (*sx + *sw > t->w){
        *sw = t->w - *sx;
    }
    if (*sy + *sh > t->h){
        *sh = t->h - *sy;
    }
    return (*sw > 0 && *sh > 0) ? 0 : -1;
}

int SDL_Init(Uint32 flags){
    (void)flags;
    SDL_QOS_ProfileReset();
    sdl_event_queue_reset();
    for (int i = 0; i < SDL_NUM_SCANCODES; i++){
        g_keyboard_state[i] = 0u;
        g_key_down_ms[i] = 0u;
        g_key_repeat_next_ms[i] = 0u;
        g_key_sym[i] = 0;
        g_key_mod[i] = KMOD_NONE;
    }
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        if (g_textures[i].alive && g_textures[i].owns_pixels){
            sdl_free_pixels(g_textures[i].pixels);
        }
        sdl_texture_free_gpu(&g_textures[i]);
        sdl_texture_free_native(&g_textures[i]);
        g_textures[i].alive = 0;
        g_textures[i].pixels = 0;
        g_textures[i].capacity = 0u;
        g_textures[i].color_r = 255u;
        g_textures[i].color_g = 255u;
        g_textures[i].color_b = 255u;
        g_textures[i].alpha_mod = 255u;
        g_textures[i].blend_mode = SDL_BLENDMODE_BLEND;
        g_textures[i].average_color = 0u;
        g_textures[i].opaque = 0;
        g_textures[i].render_hint = 0;
        g_textures[i].owns_pixels = 0;
        g_textures[i].locked = 0;
    }
    for (int i = 0; i < SDL_SHIM_MAX_SURFACES; i++){
        if (g_surfaces[i].alive && g_surfaces[i].owns_pixels){
            sdl_free_pixels(g_surfaces[i].pixels);
        }
        g_surfaces[i].alive = 0;
        g_surfaces[i].pixels = 0;
        g_surfaces[i].capacity = 0u;
        g_surfaces[i].format = 0;
        g_surfaces[i].format_storage.format = 0u;
        g_surfaces[i].format_storage.bytes_per_pixel = 0u;
        g_surfaces[i].color_key = 0u;
        g_surfaces[i].average_color = 0u;
        g_surfaces[i].color_key_enabled = 0;
        g_surfaces[i].opaque = 0;
        g_surfaces[i].render_hint = 0;
        g_surfaces[i].owns_pixels = 0;
    }
    g_window.alive = 0;
    g_renderer.alive = 0;
    sdl_soft_backbuffer_destroy();
    g_renderer.draw_alpha = 255u;
    g_renderer.draw_blend_mode = SDL_BLENDMODE_NONE;
    g_mod_state = KMOD_NONE;
    g_key_repeat_enabled = 0;
    g_key_repeat_delay_ms = SDL_SHIM_REPEAT_DELAY_MS;
    g_key_repeat_interval_ms = SDL_SHIM_REPEAT_INTERVAL_MS;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_rel_x = 0;
    g_mouse_rel_y = 0;
    g_mouse_wheel_x = 0;
    g_mouse_wheel_y = 0;
    g_mouse_buttons = 0u;
    g_pending_fill_valid = 0;
    set_error("OK");
    return 0;
}

int SDL_InitSubSystem(Uint32 flags){
    (void)flags;
    return 0;
}

void SDL_Quit(void){
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        if (g_textures[i].alive && g_textures[i].owns_pixels){
            sdl_free_pixels(g_textures[i].pixels);
        }
        sdl_texture_free_gpu(&g_textures[i]);
        sdl_texture_free_native(&g_textures[i]);
        g_textures[i].alive = 0;
        g_textures[i].pixels = 0;
        g_textures[i].capacity = 0u;
        g_textures[i].average_color = 0u;
        g_textures[i].opaque = 0;
        g_textures[i].render_hint = 0;
        g_textures[i].owns_pixels = 0;
        g_textures[i].locked = 0;
    }
    for (int i = 0; i < SDL_SHIM_MAX_SURFACES; i++){
        if (g_surfaces[i].alive && g_surfaces[i].owns_pixels){
            sdl_free_pixels(g_surfaces[i].pixels);
        }
        g_surfaces[i].alive = 0;
        g_surfaces[i].pixels = 0;
        g_surfaces[i].capacity = 0u;
        g_surfaces[i].format = 0;
        g_surfaces[i].format_storage.format = 0u;
        g_surfaces[i].format_storage.bytes_per_pixel = 0u;
        g_surfaces[i].average_color = 0u;
        g_surfaces[i].opaque = 0;
        g_surfaces[i].render_hint = 0;
        g_surfaces[i].owns_pixels = 0;
    }
    g_renderer.alive = 0;
    g_window.alive = 0;
    sdl_soft_backbuffer_destroy();
    sdl_event_queue_reset();
    for (int i = 0; i < SDL_NUM_SCANCODES; i++){
        g_keyboard_state[i] = 0u;
        g_key_down_ms[i] = 0u;
        g_key_repeat_next_ms[i] = 0u;
        g_key_sym[i] = 0;
        g_key_mod[i] = KMOD_NONE;
    }
    g_mod_state = KMOD_NONE;
    g_key_repeat_enabled = 0;
    g_key_repeat_delay_ms = SDL_SHIM_REPEAT_DELAY_MS;
    g_key_repeat_interval_ms = SDL_SHIM_REPEAT_INTERVAL_MS;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_rel_x = 0;
    g_mouse_rel_y = 0;
    g_mouse_wheel_x = 0;
    g_mouse_wheel_y = 0;
    g_mouse_buttons = 0u;
    g_pending_fill_valid = 0;
}

const char* SDL_GetError(void){
    return g_last_error;
}

char* SDL_GetBasePath(void){
    char* s = (char*)malloc(1u);
    if (!s){
        set_error("base path heap exhausted");
        return 0;
    }
    s[0] = 0;
    return s;
}

void SDL_free(void* mem){
    if (mem){
        free(mem);
    }
}

int SDL_SetHint(const char* name, const char* value){
    (void)name;
    (void)value;
    return SDL_TRUE;
}

Uint32 SDL_GetTicks(void){
    return sdl_now_ms();
}

Uint64 SDL_GetTicks64(void){
    return qos_get_time_us() / 1000ull;
}

void SDL_Delay(Uint32 ms){
    qos_sleep(ms);
}

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags){
    (void)title;
    (void)x;
    (void)y;
    g_window.w = (w > 0) ? w : (int)qos_get_screen_width();
    g_window.h = (h > 0) ? h : (int)qos_get_screen_height();
    if (g_window.w <= 0){
        g_window.w = (int)qos_get_screen_width();
    }
    if (g_window.h <= 0){
        g_window.h = (int)qos_get_screen_height();
    }
    g_window.flags = flags | SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_window.alive = 1;
    return &g_window;
}

void SDL_DestroyWindow(SDL_Window* window){
    if (window == &g_window){
        g_window.alive = 0;
    }
}

int SDL_GetCurrentDisplayMode(int index, SDL_DisplayMode* mode){
    (void)index;
    if (!mode){
        set_error("bad display mode arg");
        return -1;
    }
    mode->format = SDL_PIXELFORMAT_RGBA8888;
    mode->w = (int)qos_get_screen_width();
    mode->h = (int)qos_get_screen_height();
    if (mode->w <= 0){
        mode->w = g_window.alive ? g_window.w : 1920;
    }
    if (mode->h <= 0){
        mode->h = g_window.alive ? g_window.h : 1080;
    }
    mode->refresh_rate = 60;
    mode->driverdata = 0;
    return 0;
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags){
    (void)index;
    (void)flags;
    if (!window || !window->alive){
        set_error("window not alive");
        return 0;
    }
    g_renderer.window = window;
    g_renderer.draw_color = 0x00000000u;
    g_renderer.draw_alpha = 255u;
    g_renderer.draw_blend_mode = SDL_BLENDMODE_NONE;
    g_renderer.alive = 1;
    (void)sdl_soft_backbuffer_init(window->w, window->h);
    return &g_renderer;
}

void SDL_DestroyRenderer(SDL_Renderer* renderer){
    if (renderer == &g_renderer){
        g_renderer.alive = 0;
        sdl_soft_backbuffer_destroy();
    }
}

int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a){
    if (!renderer || !renderer->alive){
        set_error("renderer not alive");
        return -1;
    }
    renderer->draw_color = rgb_to_color(r, g, b);
    renderer->draw_alpha = a;
    return 0;
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer* renderer, SDL_BlendMode blend_mode){
    if (!renderer || !renderer->alive){
        set_error("renderer not alive");
        return -1;
    }
    renderer->draw_blend_mode = blend_mode;
    return 0;
}

int SDL_GetRenderDrawBlendMode(SDL_Renderer* renderer, SDL_BlendMode* blend_mode){
    if (!renderer || !renderer->alive || !blend_mode){
        set_error("bad renderer blend args");
        return -1;
    }
    *blend_mode = renderer->draw_blend_mode;
    return 0;
}

int SDL_RenderClear(SDL_Renderer* renderer){
    Uint64 t0 = qos_get_time_us();
    if (!renderer || !renderer->alive || !renderer->window){
        set_error("renderer/window not alive");
        return -1;
    }
    if (g_soft_fb_direct && sdl_direct_refresh_frame() == 0){
        g_pending_fill_valid = 0;
        g_sdl_profile.clear_calls++;
        g_sdl_profile.clear_us += qos_get_time_us() - t0;
        return 0;
    }
    sdl_flush_pending_fill();
    /*
     * QOS presents SDL windows as fullscreen-desktop sessions. Clearing only
     * the app-requested logical window size can make the kernel see later
     * frames as partial updates, which disables page flipping and drops back to
     * the slow full-screen copy path. Clear the whole graphics session instead.
     */
    if (g_soft_fb_direct && !g_soft_fb_direct_inactive &&
        qos_gpu2d_clear(renderer->draw_color) == 0){
        g_sdl_profile.clear_calls++;
        g_sdl_profile.clear_us += qos_get_time_us() - t0;
        return 0;
    }
    if (sdl_soft_backbuffer_valid(renderer)){
        sdl_soft_fill_rect(0u,
                           0u,
                           (unsigned int)renderer->window->w,
                           (unsigned int)renderer->window->h,
                           renderer->draw_color);
    } else{
        qos_fb_clear(renderer->draw_color);
    }
    g_sdl_profile.clear_calls++;
    g_sdl_profile.clear_us += qos_get_time_us() - t0;
    return 0;
}

int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect){
    int x, y, w, h;
    if (!renderer || !renderer->alive || !renderer->window || !rect){
        set_error("bad fill rect args");
        return -1;
    }
    if (g_soft_fb_direct && sdl_direct_refresh_frame() == 0){
        return 0;
    }
    x = rect->x;
    y = rect->y;
    w = rect->w;
    h = rect->h;
    if (w <= 0 || h <= 0){
        return 0;
    }

    if (x < 0){
        w += x;
        x = 0;
    }
    if (y < 0){
        h += y;
        y = 0;
    }
    if (x >= renderer->window->w || y >= renderer->window->h || w <= 0 || h <= 0){
        return 0;
    }
    if (x + w > renderer->window->w){
        w = renderer->window->w - x;
    }
    if (y + h > renderer->window->h){
        h = renderer->window->h - y;
    }
    if (w <= 0 || h <= 0){
        return 0;
    }

    if (renderer->draw_blend_mode == SDL_BLENDMODE_BLEND && renderer->draw_alpha == 0u){
        return 0;
    }
    return sdl_queue_fill_rect((unsigned int)x,
                               (unsigned int)y,
                               (unsigned int)w,
                               (unsigned int)h,
                               renderer->draw_color);
}

int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect){
    SDL_Rect r;
    if (!renderer || !rect || rect->w <= 0 || rect->h <= 0){
        return -1;
    }
    r = *rect;
    r.h = 1;
    (void)SDL_RenderFillRect(renderer, &r);
    r = *rect;
    r.y = rect->y + rect->h - 1;
    r.h = 1;
    (void)SDL_RenderFillRect(renderer, &r);
    r = *rect;
    r.w = 1;
    (void)SDL_RenderFillRect(renderer, &r);
    r = *rect;
    r.x = rect->x + rect->w - 1;
    r.w = 1;
    (void)SDL_RenderFillRect(renderer, &r);
    return 0;
}

int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y){
    SDL_Rect r;
    r.x = x;
    r.y = y;
    r.w = 1;
    r.h = 1;
    return SDL_RenderFillRect(renderer, &r);
}

void SDL_RenderPresent(SDL_Renderer* renderer){
    Uint64 t0 = qos_get_time_us();
    Uint64 flush_start = t0;
    if (g_soft_fb_direct){
        int was_inactive = g_soft_fb_direct_inactive;
        int active = sdl_direct_refresh_frame();
        if (active <= 0 || was_inactive){
            g_pending_fill_valid = 0;
            g_soft_fb_dirty = 0;
            g_soft_fb_direct_checked = 0;
            g_sdl_profile.present_calls++;
            g_sdl_profile.present_us += qos_get_time_us() - t0;
#ifdef QOS_PROFILE_SDL_AUTO
            sdl_profile_auto_tick();
#endif
            return;
        }
    }
    sdl_flush_pending_fill();
    g_sdl_profile.present_flush_us += qos_get_time_us() - flush_start;
    int rc = 0;
    if (sdl_soft_backbuffer_valid(renderer)){
        if (g_soft_fb_dirty && !g_soft_fb_attached){
            Uint64 upload_start = qos_get_time_us();
            rc = qos_fb_blit_native(0u,
                                    0u,
                                    (unsigned int)g_soft_fb_w,
                                    (unsigned int)g_soft_fb_h,
                                    (const unsigned int*)g_soft_fb);
            g_sdl_profile.present_upload_us += qos_get_time_us() - upload_start;
            if (rc == 0){
                g_soft_fb_dirty = 0;
            }
        }
        if (rc == 0){
            Uint64 kernel_start = qos_get_time_us();
            if (g_soft_fb_direct){
                qos_fb_direct_info_t next;
                int was_inactive = g_soft_fb_direct_inactive;
                int active = sdl_direct_refresh_frame();
                if (active <= 0 || was_inactive){
                    rc = 0;
                    g_soft_fb_dirty = 0;
                } else{
                    rc = qos_fb_direct_present(&next);
                    if (rc == 0 && next.pixels &&
                        next.width == (unsigned int)g_soft_fb_w &&
                        next.height == (unsigned int)g_soft_fb_h &&
                        next.pitch >= ((unsigned int)g_soft_fb_w * 4u)){
                        g_soft_fb = (Uint8*)next.pixels;
                        g_soft_fb_pitch = (int)next.pitch;
                        g_soft_fb_direct_inactive = 0;
                    }
                }
            } else{
                rc = qos_fb_present();
            }
            g_sdl_profile.present_kernel_us += qos_get_time_us() - kernel_start;
            if (rc == 0 && g_soft_fb_attached){
                g_soft_fb_dirty = 0;
            }
        }
    } else{
        Uint64 kernel_start = qos_get_time_us();
        rc = qos_fb_present();
        g_sdl_profile.present_kernel_us += qos_get_time_us() - kernel_start;
    }
    if (rc != 0){
        static int warned_present_failure = 0;
        set_error("fb present failed");
        if (!warned_present_failure){
            qos_puts("SDL present failed\n");
            warned_present_failure = 1;
        }
    }
    g_sdl_profile.present_calls++;
    g_sdl_profile.present_us += qos_get_time_us() - t0;
    g_soft_fb_direct_checked = 0;
#ifdef QOS_PROFILE_SDL_AUTO
    sdl_profile_auto_tick();
#endif
}

int SDL_RenderSetIntegerScale(SDL_Renderer* renderer, int enabled){
    (void)enabled;
    if (!renderer || !renderer->alive){
        set_error("renderer not alive");
        return -1;
    }
    return 0;
}

void SDL_RenderSetViewport(SDL_Renderer* renderer, const SDL_Rect* rect){
    (void)renderer;
    (void)rect;
}

int SDL_PollEvent(SDL_Event* event){
    Uint64 t0 = qos_get_time_us();
    int ret = 0;
    qos_event_t qos_ev;
    SDL_Event translated;
    int drained = 0;
    if (!event){
        return 0;
    }
    sdl_event_clear(event);

    if (sdl_event_pop(event)){
        ret = 1;
        goto done;
    }

    if (qos_poll_event(&qos_ev) <= 0){
        ret = sdl_poll_repeat_event(event);
        goto done;
    }

    if (sdl_translate_qos_event(&qos_ev, event)){
        sdl_queue_text_input_if_printable(&qos_ev);
        ret = 1;
    }

    while (drained < 8 && qos_poll_event(&qos_ev) > 0){
        drained++;
        if (sdl_translate_qos_event(&qos_ev, &translated)){
            (void)sdl_event_push(&translated);
            sdl_queue_text_input_if_printable(&qos_ev);
        }
    }
    if (ret){
        goto done;
    }
    ret = sdl_poll_repeat_event(event);
done:
    g_sdl_profile.poll_calls++;
    g_sdl_profile.poll_us += qos_get_time_us() - t0;
    return ret;
}

void SDL_PumpEvents(void){
    Uint64 t0 = qos_get_time_us();
    qos_event_t qos_ev;
    SDL_Event event;
    int guard = 0;
    while (guard < SDL_SHIM_EVENT_QUEUE_SIZE &&
           qos_poll_event(&qos_ev) > 0){
        guard++;
        if (sdl_translate_qos_event(&qos_ev, &event)){
            (void)sdl_event_push(&event);
            sdl_queue_text_input_if_printable(&qos_ev);
        }
    }
    g_sdl_profile.poll_calls++;
    g_sdl_profile.poll_us += qos_get_time_us() - t0;
}

const Uint8* SDL_GetKeyboardState(int* numkeys){
    if (numkeys){
        *numkeys = SDL_NUM_SCANCODES;
    }
    return g_keyboard_state;
}

const Uint8* SDL_GetKeyState(int* numkeys){
    return SDL_GetKeyboardState(numkeys);
}

Uint16 SDL_GetModState(void){
    return g_mod_state;
}

void SDL_SetModState(Uint16 modstate){
    g_mod_state = modstate;
}

int SDL_EnableKeyRepeat(int delay_ms, int interval_ms){
    if (delay_ms <= 0 || interval_ms <= 0){
        g_key_repeat_enabled = 0;
        return 0;
    }
    g_key_repeat_enabled = 1;
    g_key_repeat_delay_ms = (Uint32)delay_ms;
    g_key_repeat_interval_ms = (Uint32)interval_ms;
    return 0;
}

void SDL_QOS_SetKeyRepeat(int enabled, Uint32 delay_ms, Uint32 interval_ms){
    g_key_repeat_enabled = enabled ? 1 : 0;
    if (delay_ms != 0u){
        g_key_repeat_delay_ms = delay_ms;
    }
    if (interval_ms != 0u){
        g_key_repeat_interval_ms = interval_ms;
    }
}

Uint32 SDL_GetMouseState(int* x, int* y){
    if (x){
        *x = g_mouse_x;
    }
    if (y){
        *y = g_mouse_y;
    }
    return g_mouse_buttons;
}

Uint32 SDL_GetRelativeMouseState(int* x, int* y){
    if (x){
        *x = g_mouse_rel_x;
    }
    if (y){
        *y = g_mouse_rel_y;
    }
    g_mouse_rel_x = 0;
    g_mouse_rel_y = 0;
    return g_mouse_buttons;
}

Uint32 SDL_GetGlobalMouseState(int* x, int* y){
    return SDL_GetMouseState(x, y);
}

int SDL_QOS_GetMouseWheel(int* x, int* y){
    if (x){
        *x = g_mouse_wheel_x;
    }
    if (y){
        *y = g_mouse_wheel_y;
    }
    g_mouse_wheel_x = 0;
    g_mouse_wheel_y = 0;
    return 0;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h){
    Uint32 bytes;
    SDL_Texture* t;

    if (!renderer || !renderer->alive){
        set_error("renderer not alive");
        return 0;
    }
    if (w <= 0 || h <= 0){
        set_error("bad texture size");
        return 0;
    }
    if ((Uint32)w > 32767u || (Uint32)h > 32767u){
        set_error("texture too large");
        return 0;
    }
    if (format == 0u){
        format = SDL_PIXELFORMAT_RGBA8888;
    }
    if (format != SDL_PIXELFORMAT_RGBA8888){
        set_error("only RGBA8888 supported");
        return 0;
    }
    if ((Uint32)w > (0xFFFFFFFFu / (Uint32)h) / 4u){
        set_error("texture size overflow");
        return 0;
    }
    bytes = (Uint32)w * (Uint32)h * 4u;

    t = alloc_texture_slot();
    if (!t){
        set_error("texture slots exhausted");
        return 0;
    }
    t->pixels = sdl_alloc_pixels(bytes);
    if (!t->pixels){
        set_error("texture heap exhausted");
        return 0;
    }

    t->w = w;
    t->h = h;
    t->format = format;
    t->access = (Uint32)access;
    t->pitch = w * 4;
    t->capacity = bytes;
    t->native_pixels = 0;
    t->native_capacity = 0u;
    t->gpu_texture_id = 0u;
    t->native_valid = 0;
    t->gpu_texture_valid = 0;
    t->color_r = 255u;
    t->color_g = 255u;
    t->color_b = 255u;
    t->alpha_mod = 255u;
    t->blend_mode = SDL_BLENDMODE_BLEND;
    t->average_color = 0u;
    t->opaque = 0;
    t->render_hint = 0;
    t->owns_pixels = 1;
    t->locked = 0;
    t->alive = 1;
    sdl_zero_bytes(t->pixels, bytes);
    return t;
}

SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface){
    SDL_Texture* t;
    Uint32 row_bytes;
    Uint32 expected_capacity;
    int adopted_pixels = 0;
    Uint64 t0 = qos_get_time_us();
    if (!renderer || !renderer->alive){
        set_error("renderer not alive");
        return 0;
    }
    if (!surface || !surface->alive || !surface->pixels || surface->w <= 0 || surface->h <= 0){
        set_error("surface not alive");
        return 0;
    }
    if (surface_format_value(surface) != SDL_PIXELFORMAT_RGBA8888){
        set_error("surface format unsupported");
        return 0;
    }
    if ((Uint32)surface->w > (0xFFFFFFFFu / (Uint32)surface->h) / 4u){
        set_error("surface size overflow");
        return 0;
    }
    row_bytes = (Uint32)surface->w * 4u;
    expected_capacity = row_bytes * (Uint32)surface->h;
    if (surface->pitch < (int)row_bytes || surface->capacity < expected_capacity){
        set_error("surface storage invalid");
        return 0;
    }

    t = alloc_texture_slot();
    if (!t){
        set_error("texture slots exhausted");
        return 0;
    }

    t->w = surface->w;
    t->h = surface->h;
    t->format = surface_format_value(surface);
    t->access = SDL_TEXTUREACCESS_STATIC;
    t->pitch = (int)row_bytes;
    t->capacity = expected_capacity;
    t->native_pixels = 0;
    t->native_capacity = 0u;
    t->gpu_texture_id = 0u;
    t->native_valid = 0;
    t->gpu_texture_valid = 0;

    if (surface->owns_pixels &&
        surface->pitch == (int)row_bytes &&
        surface->capacity == expected_capacity){
        /*
         * IMG_LoadTexture() immediately frees the temporary surface. Transfer
         * ownership for the common BMP path to avoid a large duplicate copy and
         * reduce peak heap pressure during asset loading.
         */
        t->pixels = surface->pixels;
        surface->owns_pixels = 0;
        adopted_pixels = 1;
    } else{
        t->pixels = sdl_alloc_pixels(t->capacity);
        if (!t->pixels){
            set_error("texture heap exhausted");
            return 0;
        }
        for (int y = 0; y < surface->h; y++){
            Uint8* dst_row = t->pixels + ((Uint32)y * row_bytes);
            const Uint8* src_row = surface->pixels + ((Uint32)y * (Uint32)surface->pitch);
            sdl_copy_bytes(dst_row, src_row, row_bytes);
        }
    }
    t->color_r = 255u;
    t->color_g = 255u;
    t->color_b = 255u;
    t->alpha_mod = 255u;
    t->blend_mode = SDL_BLENDMODE_BLEND;
    t->average_color = surface->average_color;
    t->opaque = surface->opaque;
    t->render_hint = surface->render_hint;
    t->owns_pixels = 1;
    t->locked = 0;
    t->alive = 1;
    if (t->opaque){
        (void)sdl_texture_ensure_native(t);
    }
    (void)sdl_texture_upload_gpu(t);
    if (adopted_pixels){
        surface->pixels = t->pixels;
    }
    g_sdl_profile.texture_calls++;
    g_sdl_profile.texture_bytes += (Uint64)t->capacity;
    g_sdl_profile.texture_us += qos_get_time_us() - t0;
    return t;
}

void SDL_DestroyTexture(SDL_Texture* texture){
    if (!texture){
        return;
    }
    if (texture->alive && texture->owns_pixels){
        sdl_free_pixels(texture->pixels);
    }
    sdl_texture_free_gpu(texture);
    sdl_texture_free_native(texture);
    texture->alive = 0;
    texture->locked = 0;
    texture->pixels = 0;
    texture->capacity = 0u;
    texture->average_color = 0u;
    texture->opaque = 0;
    texture->render_hint = 0;
    texture->owns_pixels = 0;
}

int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access, int* w, int* h){
    if (!texture || !texture->alive){
        set_error("texture not alive");
        return -1;
    }
    if (format){
        *format = texture->format;
    }
    if (access){
        *access = (int)texture->access;
    }
    if (w){
        *w = texture->w;
    }
    if (h){
        *h = texture->h;
    }
    return 0;
}

int SDL_SetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode blend_mode){
    if (!texture || !texture->alive){
        set_error("texture not alive");
        return -1;
    }
    texture->blend_mode = blend_mode;
    return 0;
}

int SDL_GetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode* blend_mode){
    if (!texture || !texture->alive || !blend_mode){
        set_error("bad texture blend args");
        return -1;
    }
    *blend_mode = texture->blend_mode;
    return 0;
}

int SDL_SetTextureAlphaMod(SDL_Texture* texture, Uint8 alpha){
    if (!texture || !texture->alive){
        set_error("texture not alive");
        return -1;
    }
    texture->alpha_mod = alpha;
    return 0;
}

int SDL_GetTextureAlphaMod(SDL_Texture* texture, Uint8* alpha){
    if (!texture || !texture->alive || !alpha){
        set_error("bad alpha mod args");
        return -1;
    }
    *alpha = texture->alpha_mod;
    return 0;
}

int SDL_SetTextureColorMod(SDL_Texture* texture, Uint8 r, Uint8 g, Uint8 b){
    if (!texture || !texture->alive){
        set_error("texture not alive");
        return -1;
    }
    texture->color_r = r;
    texture->color_g = g;
    texture->color_b = b;
    return 0;
}

int SDL_GetTextureColorMod(SDL_Texture* texture, Uint8* r, Uint8* g, Uint8* b){
    if (!texture || !texture->alive){
        set_error("texture not alive");
        return -1;
    }
    if (r){
        *r = texture->color_r;
    }
    if (g){
        *g = texture->color_g;
    }
    if (b){
        *b = texture->color_b;
    }
    return 0;
}

int SDL_LockTexture(SDL_Texture* texture, const SDL_Rect* rect, void** pixels, int* pitch){
    int rx = 0;
    int ry = 0;
    int rw = 0;
    int rh = 0;
    if (!texture || !texture->alive || !pixels || !pitch || !texture->pixels){
        set_error("bad lock texture args");
        return -1;
    }

    rw = texture->w;
    rh = texture->h;
    if (rect){
        rx = rect->x;
        ry = rect->y;
        rw = rect->w;
        rh = rect->h;
        if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || rx + rw > texture->w || ry + rh > texture->h){
            set_error("unsupported lock rect");
            return -1;
        }
    }

    texture->locked = 1;
    sdl_texture_invalidate_native(texture);
    *pitch = texture->pitch;
    *pixels = (void*)(texture->pixels + ((Uint32)ry * (Uint32)texture->pitch) + ((Uint32)rx * 4u));
    return 0;
}

void SDL_UnlockTexture(SDL_Texture* texture){
    if (texture){
        texture->locked = 0;
    }
}

int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch){
    int rx = 0;
    int ry = 0;
    int rw = 0;
    int rh = 0;
    int spitch = pitch;
    const Uint8* src = (const Uint8*)pixels;
    if (!texture || !texture->alive || !texture->pixels || !pixels){
        set_error("bad update texture args");
        return -1;
    }

    rw = texture->w;
    rh = texture->h;
    if (rect){
        rx = rect->x;
        ry = rect->y;
        rw = rect->w;
        rh = rect->h;
    }
    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || rx + rw > texture->w || ry + rh > texture->h){
        set_error("bad update rect");
        return -1;
    }
    if (spitch <= 0){
        spitch = rw * 4;
    }
    if (spitch < rw * 4){
        set_error("bad update pitch");
        return -1;
    }

    for (int y = 0; y < rh; y++){
        Uint8* dst_row = texture->pixels +
                         ((Uint32)(ry + y) * (Uint32)texture->pitch) +
                         ((Uint32)rx * 4u);
        const Uint8* src_row = src + ((Uint32)y * (Uint32)spitch);
        sdl_copy_bytes(dst_row, src_row, (Uint32)rw * 4u);
    }
    sdl_texture_invalidate_native(texture);
    return 0;
}

SDL_Surface* SDL_LoadBMP(const char* file){
    int n;
    Uint32 pixel_offset;
    Uint32 dib_size;
    int width;
    int height_signed;
    int height;
    int top_down = 0;
    Uint16 planes;
    Uint16 bpp;
    Uint32 compression;
    Uint32 row_stride;
    Uint32 pixel_bytes;
    SDL_Surface* s;
    int alpha_nonzero = 0;
    int opaque = 1;
    unsigned long long sum_r = 0ull;
    unsigned long long sum_g = 0ull;
    unsigned long long sum_b = 0ull;
    unsigned int sum_count = 0u;
    Uint8* bmp = 0;
    Uint64 profile_total0 = qos_get_time_us();
    Uint64 profile_read_us = 0ull;
    Uint64 profile_decode_us = 0ull;
    Uint64 profile_t0;

    if (!file || !*file){
        sdl_profile_note_bmp(0, 0, 0ull, 0ull, qos_get_time_us() - profile_total0);
        set_error("bad BMP filename");
        return 0;
    }

    bmp = (Uint8*)malloc(SDL_SHIM_BMP_FILE_MAX);
    if (!bmp){
        sdl_profile_note_bmp(0, 0, 0ull, 0ull, qos_get_time_us() - profile_total0);
        set_error("BMP temp heap exhausted");
        return 0;
    }

    profile_t0 = qos_get_time_us();
    n = qos_file_read_bmp(file, bmp, SDL_SHIM_BMP_FILE_MAX);
    profile_read_us = qos_get_time_us() - profile_t0;
    if (n < 54){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("BMP read failed");
        return 0;
    }
    if (bmp[0] != 'B' || bmp[1] != 'M'){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("not a BMP");
        return 0;
    }

    pixel_offset = read_le32(&bmp[10]);
    dib_size = read_le32(&bmp[14]);
    if (dib_size < 40u || pixel_offset >= (Uint32)n){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("unsupported BMP header");
        return 0;
    }

    width = read_s32_le(&bmp[18]);
    height_signed = read_s32_le(&bmp[22]);
    planes = read_le16(&bmp[26]);
    bpp = read_le16(&bmp[28]);
    compression = read_le32(&bmp[30]);

    if (width <= 0 || height_signed == 0 || planes != 1u ||
        (bpp != 24u && bpp != 32u) || compression != 0u){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("unsupported BMP format");
        return 0;
    }
    if (height_signed < 0){
        top_down = 1;
        height = -height_signed;
    } else{
        height = height_signed;
    }
    if (height <= 0 || width > 2048 || height > 2048){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("BMP dimensions unsupported");
        return 0;
    }

    row_stride = ((((Uint32)width * (Uint32)bpp) + 31u) / 32u) * 4u;
    if (row_stride == 0u ||
        (Uint32)height > (0xFFFFFFFFu - pixel_offset) / row_stride){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("BMP row overflow");
        return 0;
    }
    if (pixel_offset + ((Uint32)height * row_stride) > (Uint32)n){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("BMP truncated");
        return 0;
    }
    if ((Uint32)width > (0xFFFFFFFFu / (Uint32)height) / 4u){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("BMP size overflow");
        return 0;
    }
    pixel_bytes = (Uint32)width * (Uint32)height * 4u;

    s = alloc_surface_slot();
    if (!s){
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("surface slots exhausted");
        return 0;
    }
    s->pixels = sdl_alloc_pixels(pixel_bytes);
    if (!s->pixels){
        qos_puts("SDL_LoadBMP: surface heap exhausted bytes=");
        sdl_profile_put_u64((Uint64)pixel_bytes);
        qos_puts(" largest=");
        sdl_profile_put_u64((Uint64)qos_heap_largest_free());
        qos_puts(" free=");
        sdl_profile_put_u64((Uint64)qos_heap_free());
        qos_puts("\n");
        free(bmp);
        sdl_profile_note_bmp(0, n, profile_read_us, 0ull, qos_get_time_us() - profile_total0);
        set_error("surface heap exhausted");
        return 0;
    }

    s->w = width;
    s->h = height;
    s->pitch = width * 4;
    surface_set_format(s, SDL_PIXELFORMAT_RGBA8888);
    s->capacity = pixel_bytes;
    s->color_key = 0u;
    s->average_color = 0u;
    s->color_key_enabled = 0;
    s->opaque = 1;
    if (sdl_path_has_char_hint(file)){
        s->render_hint = SDL_SHIM_RENDER_HINT_CHAR_16;
    } else if (sdl_path_has_tile_hint(file)){
        s->render_hint = SDL_SHIM_RENDER_HINT_TILE_FILL;
    } else{
        s->render_hint = 0;
    }
    s->owns_pixels = 1;
    s->alive = 1;

    profile_t0 = qos_get_time_us();
    for (int y = 0; y < height; y++){
        int src_y = top_down ? y : (height - 1 - y);
        const Uint8* src_row = bmp + pixel_offset + ((Uint32)src_y * row_stride);
        Uint8* dst_row = s->pixels + ((Uint32)y * (Uint32)s->pitch);
        for (int x = 0; x < width; x++){
            const Uint8* sp = src_row + ((Uint32)x * ((Uint32)bpp / 8u));
            Uint8* dp = dst_row + ((Uint32)x * 4u);
            dp[0] = sp[2];
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = (bpp == 32u) ? sp[3] : 255u;
            if (dp[3] != 255u){
                opaque = 0;
            }
            if (dp[3] != 0u){
                alpha_nonzero = 1;
                sum_r += dp[0];
                sum_g += dp[1];
                sum_b += dp[2];
                sum_count++;
            }
        }
    }

    if (bpp == 32u && !alpha_nonzero){
        for (Uint32 i = 0u; i < pixel_bytes; i += 4u){
            s->pixels[i + 3u] = 255u;
        }
        opaque = 1;
    }
    if (sum_count == 0u){
        sum_count = 1u;
    }
    s->average_color = rgb_to_color((Uint8)(sum_r / sum_count),
                                    (Uint8)(sum_g / sum_count),
                                    (Uint8)(sum_b / sum_count));
    s->opaque = opaque;
    profile_decode_us = qos_get_time_us() - profile_t0;

    free(bmp);
    sdl_profile_note_bmp(1, n, profile_read_us, profile_decode_us, qos_get_time_us() - profile_total0);
    return s;
}

void SDL_FreeSurface(SDL_Surface* surface){
    if (!surface){
        return;
    }
    if (surface->alive && surface->owns_pixels){
        sdl_free_pixels(surface->pixels);
    }
    surface->alive = 0;
    surface->pixels = 0;
    surface->capacity = 0u;
    surface->format = 0;
    surface->format_storage.format = 0u;
    surface->format_storage.bytes_per_pixel = 0u;
    surface->average_color = 0u;
    surface->opaque = 0;
    surface->render_hint = 0;
    surface->owns_pixels = 0;
}

void SDL_DestroySurface(SDL_Surface* surface){
    SDL_FreeSurface(surface);
}

SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format){
    (void)flags;
    Uint32 bytes;
    SDL_Surface* s;

    if (w <= 0 || h <= 0 || depth != 32){
        set_error("bad surface size/depth");
        return 0;
    }
    if (format == 0u){
        format = SDL_PIXELFORMAT_RGBA8888;
    }
    if (format != SDL_PIXELFORMAT_RGBA8888){
        set_error("only RGBA8888 surfaces supported");
        return 0;
    }
    if ((Uint32)w > (0xFFFFFFFFu / (Uint32)h) / 4u){
        set_error("surface size overflow");
        return 0;
    }
    bytes = (Uint32)w * (Uint32)h * 4u;

    s = alloc_surface_slot();
    if (!s){
        set_error("surface slots exhausted");
        return 0;
    }

    s->pixels = sdl_alloc_pixels(bytes);
    if (!s->pixels){
        set_error("surface heap exhausted");
        return 0;
    }
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    surface_set_format(s, format);
    s->capacity = bytes;
    s->color_key = 0u;
    s->average_color = 0u;
    s->color_key_enabled = 0;
    s->opaque = 1;
    s->render_hint = 0;
    s->owns_pixels = 1;
    s->alive = 1;
    sdl_zero_bytes(s->pixels, bytes);
    return s;
}

int SDL_FillRect(SDL_Surface* surface, const SDL_Rect* rect, Uint32 color){
    int x = 0;
    int y = 0;
    int w;
    int h;
    Uint8 r = color_r(color);
    Uint8 g = color_g(color);
    Uint8 b = color_b(color);
    Uint8 a = color_a(color);

    if (!surface || !surface->alive || !surface->pixels){
        set_error("surface not alive");
        return -1;
    }
    w = surface->w;
    h = surface->h;
    if (rect){
        x = rect->x;
        y = rect->y;
        w = rect->w;
        h = rect->h;
    }
    if (w <= 0 || h <= 0){
        return 0;
    }
    if (x < 0){
        w += x;
        x = 0;
    }
    if (y < 0){
        h += y;
        y = 0;
    }
    if (x >= surface->w || y >= surface->h || w <= 0 || h <= 0){
        return 0;
    }
    if (x + w > surface->w){
        w = surface->w - x;
    }
    if (y + h > surface->h){
        h = surface->h - y;
    }

    for (int yy = 0; yy < h; yy++){
        Uint8* row = surface->pixels +
                     ((Uint32)(y + yy) * (Uint32)surface->pitch) +
                     ((Uint32)x * 4u);
        for (int xx = 0; xx < w; xx++){
            Uint8* p = row + ((Uint32)xx * 4u);
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
    }
    if (x == 0 && y == 0 && w == surface->w && h == surface->h){
        surface->average_color = rgb_to_color(r, g, b);
        surface->opaque = (a == 255u) ? 1 : 0;
    }
    return 0;
}

static void apply_surface_color_key(SDL_Surface* surface){
    if (!surface || !surface->alive || !surface->pixels || !surface->color_key_enabled){
        return;
    }

    Uint8 kr = color_r(surface->color_key);
    Uint8 kg = color_g(surface->color_key);
    Uint8 kb = color_b(surface->color_key);
    unsigned long long sr = 0ull;
    unsigned long long sg = 0ull;
    unsigned long long sb = 0ull;
    unsigned int count = 0u;
    int opaque = 1;
    for (int y = 0; y < surface->h; y++){
        Uint8* row = surface->pixels + ((Uint32)y * (Uint32)surface->pitch);
        for (int x = 0; x < surface->w; x++){
            Uint8* p = row + ((Uint32)x * 4u);
            if (p[0] == kr && p[1] == kg && p[2] == kb){
                p[3] = 0u;
            }
            if (p[3] != 255u){
                opaque = 0;
            }
            if (p[3] != 0u){
                sr += p[0];
                sg += p[1];
                sb += p[2];
                count++;
            }
        }
    }
    if (count == 0u){
        count = 1u;
    }
    surface->average_color = rgb_to_color((Uint8)(sr / count),
                                          (Uint8)(sg / count),
                                          (Uint8)(sb / count));
    surface->opaque = opaque;
}

int SDL_SetSurfaceColorKey(SDL_Surface* surface, int enabled, Uint32 key){
    if (!surface || !surface->alive || !surface->pixels){
        set_error("surface not alive");
        return -1;
    }
    surface->color_key = key;
    surface->color_key_enabled = enabled ? 1 : 0;
    if (surface->color_key_enabled){
        apply_surface_color_key(surface);
        surface->opaque = 0;
    }
    return 0;
}

int SDL_SetColorKey(SDL_Surface* surface, int flag, Uint32 key){
    return SDL_SetSurfaceColorKey(surface, flag, key);
}

int SDL_GetSurfaceColorKey(SDL_Surface* surface, Uint32* key){
    if (!surface || !surface->alive || !key){
        set_error("bad color key args");
        return -1;
    }
    *key = surface->color_key;
    return surface->color_key_enabled ? 0 : -1;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b){
    (void)format;
    return rgba_to_color(r, g, b, 255u);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b, Uint8 a){
    (void)format;
    return rgba_to_color(r, g, b, a);
}

static int sdl_render_copy_internal(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst, int flip){
    int sx, sy, sw, sh;
    int dx, dy, dw, dh;
    int screen_w, screen_h;

    if (!renderer || !renderer->alive || !renderer->window){
        set_error("renderer not alive");
        return -1;
    }
    if (!texture || !texture->alive || !texture->pixels){
        set_error("texture not alive");
        return -1;
    }
    if (g_soft_fb_direct && sdl_direct_refresh_frame() == 0){
        return 0;
    }

    sx = 0; sy = 0; sw = texture->w; sh = texture->h;
    if (src){
        sx = src->x;
        sy = src->y;
        sw = src->w;
        sh = src->h;
    }
    if (clamp_src_rect(texture, &sx, &sy, &sw, &sh) != 0){
        set_error("bad source rect");
        return -1;
    }

    if (dst){
        dx = dst->x;
        dy = dst->y;
        dw = dst->w;
        dh = dst->h;
    } else{
        dx = 0;
        dy = 0;
        dw = renderer->window->w;
        dh = renderer->window->h;
    }
    if (dw <= 0 || dh <= 0){
        return 0;
    }

    screen_w = renderer->window->w;
    screen_h = renderer->window->h;
    if (screen_w <= 0 || screen_h <= 0){
        return 0;
    }

    int visible_x0 = 0;
    int visible_y0 = 0;
    int visible_x1 = dw;
    int visible_y1 = dh;
    if (dx < 0){
        visible_x0 = -dx;
    }
    if (dy < 0){
        visible_y0 = -dy;
    }
    if (dx + visible_x1 > screen_w){
        visible_x1 = screen_w - dx;
    }
    if (dy + visible_y1 > screen_h){
        visible_y1 = screen_h - dy;
    }
    if (visible_x1 <= visible_x0 || visible_y1 <= visible_y0){
        return 0;
    }
    g_sdl_profile.rendercopy_pixels +=
        (Uint64)(visible_x1 - visible_x0) * (Uint64)(visible_y1 - visible_y0);

    if (sdl_gpu2d_status_cached() & QOS_GPU2D_CAP_ACCEL_BLIT){
        int gpu2d_rc = sdl_gpu2d_try_blit(texture,
                                          sx,
                                          sy,
                                          sw,
                                          sh,
                                          dx,
                                          dy,
                                          dw,
                                          dh,
                                          visible_x0,
                                          visible_y0,
                                          visible_x1,
                                          visible_y1,
                                          flip);
        if (gpu2d_rc == 0){
            g_sdl_profile.rendercopy_gpu2d_calls++;
            return 0;
        }
        g_sdl_profile.rendercopy_gpu2d_miss++;
    }

    if (sdl_soft_backbuffer_valid(renderer)){
#if SDL_SHIM_ENABLE_TILE_FILL_FASTPATH
        if (texture->render_hint == SDL_SHIM_RENDER_HINT_TILE_FILL &&
            texture->opaque &&
            texture->alpha_mod == 255u &&
            texture->color_r == 255u &&
            texture->color_g == 255u &&
            texture->color_b == 255u &&
            sx == 0 && sy == 0 && sw == texture->w && sh == texture->h &&
            dw <= 64 && dh <= 64){
            g_sdl_profile.rendercopy_fill_calls++;
            return sdl_queue_fill_rect((unsigned int)(dx + visible_x0),
                                       (unsigned int)(dy + visible_y0),
                                       (unsigned int)(visible_x1 - visible_x0),
                                       (unsigned int)(visible_y1 - visible_y0),
                                       texture->average_color);
        }
#endif
        if (sdl_soft_blit_texture(texture,
                                  sx,
                                  sy,
                                  sw,
                                  sh,
                                  dx,
                                  dy,
                                  dw,
                                  dh,
                                  visible_x0,
                                  visible_y0,
                                  visible_x1,
                                  visible_y1,
                                  flip) != 0){
            set_error("soft blit failed");
            return -1;
        }
        g_sdl_profile.rendercopy_soft_calls++;
        return 0;
    }

#if SDL_SHIM_ENABLE_TILE_FILL_FASTPATH
    if (texture->render_hint == SDL_SHIM_RENDER_HINT_TILE_FILL &&
        texture->opaque &&
        texture->alpha_mod == 255u &&
        texture->color_r == 255u &&
        texture->color_g == 255u &&
        texture->color_b == 255u &&
        sx == 0 && sy == 0 && sw == texture->w && sh == texture->h &&
        dw <= 64 && dh <= 64){
        g_sdl_profile.rendercopy_fill_calls++;
        return sdl_queue_fill_rect((unsigned int)(dx + visible_x0),
                                   (unsigned int)(dy + visible_y0),
                                   (unsigned int)(visible_x1 - visible_x0),
                                   (unsigned int)(visible_y1 - visible_y0),
                                   texture->average_color);
    }
#endif

    if (flip == SDL_FLIP_NONE &&
        visible_x0 == 0 && visible_y0 == 0 &&
        visible_x1 == dw && visible_y1 == dh &&
        sx == 0 && sy == 0 &&
        sw == texture->w && sh == texture->h &&
        dw == texture->w && dh == texture->h &&
        texture->alpha_mod == 255u &&
        texture->color_r == 255u &&
        texture->color_g == 255u &&
        texture->color_b == 255u &&
        texture->opaque &&
        sdl_texture_ensure_native(texture) == 0){
        sdl_flush_pending_fill();
        if (qos_fb_blit_native((unsigned int)dx,
                               (unsigned int)dy,
                               (unsigned int)dw,
                               (unsigned int)dh,
                               texture->native_pixels) != 0){
            set_error("fb native blit failed");
            return -1;
        }
        g_sdl_profile.rendercopy_direct_calls++;
        g_sdl_profile.rendercopy_native_calls++;
        return 0;
    }

    if (flip == SDL_FLIP_NONE &&
        visible_x0 == 0 && visible_y0 == 0 &&
        visible_x1 == dw && visible_y1 == dh &&
        sx == 0 && sy == 0 &&
        sw == texture->w && sh == texture->h &&
        dw == texture->w && dh == texture->h &&
        texture->alpha_mod == 255u &&
        texture->color_r == 255u &&
        texture->color_g == 255u &&
        texture->color_b == 255u &&
        texture->blend_mode != SDL_BLENDMODE_NONE){
        sdl_flush_pending_fill();
        if (qos_fb_blit_rgba((unsigned int)dx,
                             (unsigned int)dy,
                             (unsigned int)dw,
                             (unsigned int)dh,
                             texture->pixels) != 0){
            set_error("fb blit failed");
            return -1;
        }
        g_sdl_profile.rendercopy_direct_calls++;
        return 0;
    }

    {
        unsigned int out_w = (unsigned int)(visible_x1 - visible_x0);
        unsigned int out_h = (unsigned int)(visible_y1 - visible_y0);
        unsigned long total_bytes = (unsigned long)out_w * (unsigned long)out_h * 4ul;
        if (total_bytes > 0ul && total_bytes <= (unsigned long)SDL_SHIM_BLITBUF_BYTES){
            sdl_flush_pending_fill();
            if (flip == SDL_FLIP_NONE &&
                visible_x0 == 0 && visible_y0 == 0 &&
                visible_x1 == dw && visible_y1 == dh &&
                sx == 0 && sy == 0 &&
                sw == texture->w && sh == texture->h &&
                dw == texture->w && dh == texture->h &&
                texture->opaque &&
                texture->alpha_mod == 255u &&
                texture->color_r == 255u &&
                texture->color_g == 255u &&
                texture->color_b == 255u &&
                sdl_texture_ensure_native(texture) == 0){
                Uint32* native_out = g_native_blitbuf;
                for (unsigned int by = 0u; by < out_h; by++){
                    int oy = visible_y0 + (int)by;
                    for (unsigned int bx = 0u; bx < out_w; bx++){
                        int ox = visible_x0 + (int)bx;
                        int src_ox = (flip & SDL_FLIP_HORIZONTAL) ? (dw - 1 - ox) : ox;
                        int src_oy = (flip & SDL_FLIP_VERTICAL) ? (dh - 1 - oy) : oy;
                        int tx_x = sx + (int)(((unsigned long long)src_ox * (unsigned long long)sw) /
                                              (unsigned long long)dw);
                        int tx_y = sy + (int)(((unsigned long long)src_oy * (unsigned long long)sh) /
                                              (unsigned long long)dh);
                        native_out[(by * out_w) + bx] =
                            texture->native_pixels[((Uint32)tx_y * (Uint32)texture->w) +
                                                   (Uint32)tx_x];
                    }
                }
                if (qos_fb_blit_native((unsigned int)(dx + visible_x0),
                                       (unsigned int)(dy + visible_y0),
                                       out_w,
                                       out_h,
                                       native_out) != 0){
                    set_error("fb native blit failed");
                    return -1;
                }
                g_sdl_profile.rendercopy_native_calls++;
                g_sdl_profile.rendercopy_blitbuf_calls++;
                return 0;
            }
            for (unsigned int by = 0u; by < out_h; by++){
                int oy = visible_y0 + (int)by;
                for (unsigned int bx = 0u; bx < out_w; bx++){
                    int ox = visible_x0 + (int)bx;
                    int src_ox = (flip & SDL_FLIP_HORIZONTAL) ? (dw - 1 - ox) : ox;
                    int src_oy = (flip & SDL_FLIP_VERTICAL) ? (dh - 1 - oy) : oy;
                    int tx_x = sx + (int)(((unsigned long long)src_ox * (unsigned long long)sw) / (unsigned long long)dw);
                    int tx_y = sy + (int)(((unsigned long long)src_oy * (unsigned long long)sh) / (unsigned long long)dh);
                    const Uint8* sp = texture->pixels +
                                      ((Uint32)tx_y * (Uint32)texture->pitch) +
                                      ((Uint32)tx_x * 4u);
                    Uint8* dp = &g_blitbuf[((by * out_w) + bx) * 4u];
                    dp[0] = (Uint8)(((Uint32)sp[0] * (Uint32)texture->color_r) / 255u);
                    dp[1] = (Uint8)(((Uint32)sp[1] * (Uint32)texture->color_g) / 255u);
                    dp[2] = (Uint8)(((Uint32)sp[2] * (Uint32)texture->color_b) / 255u);
                    if (texture->blend_mode == SDL_BLENDMODE_NONE){
                        dp[3] = 255u;
                    } else{
                        dp[3] = (Uint8)(((Uint32)sp[3] * (Uint32)texture->alpha_mod) / 255u);
                    }
                }
            }
            if (qos_fb_blit_rgba((unsigned int)(dx + visible_x0),
                                 (unsigned int)(dy + visible_y0),
                                 out_w,
                                 out_h,
                                 g_blitbuf) != 0){
                set_error("fb blit failed");
                return -1;
            }
            g_sdl_profile.rendercopy_blitbuf_calls++;
            return 0;
        }
    }

    sdl_flush_pending_fill();
    g_sdl_profile.rendercopy_row_calls++;
    for (int oy = 0; oy < dh; oy++){
        int py = dy + oy;
        int tx_y;
        int run_start = 0;

        if (py < 0 || py >= screen_h){
            continue;
        }
        while (run_start < dw){
            int run_len = sdl_i_min(dw - run_start, (int)SDL_SHIM_ROWBUF_PIXELS);
            int visible_from = run_start;
            int visible_to = run_start + run_len;

            if (dx + visible_from < 0){
                visible_from = -dx;
            }
            if (dx + visible_to > screen_w){
                visible_to = screen_w - dx;
            }
            if (visible_to > visible_from){
                int out_n = visible_to - visible_from;
                for (int i = 0; i < out_n; i++){
                    int ox = visible_from + i;
                    int src_ox = (flip & SDL_FLIP_HORIZONTAL) ? (dw - 1 - ox) : ox;
                    int src_oy = (flip & SDL_FLIP_VERTICAL) ? (dh - 1 - oy) : oy;
                    int tx_x = sx + (int)(((unsigned long long)src_ox * (unsigned long long)sw) / (unsigned long long)dw);
                    tx_y = sy + (int)(((unsigned long long)src_oy * (unsigned long long)sh) / (unsigned long long)dh);
                    const Uint8* sp = texture->pixels +
                                      ((Uint32)tx_y * (Uint32)texture->pitch) +
                                      ((Uint32)tx_x * 4u);
                    Uint8* dp = &g_rowbuf[i * 4];
                    dp[0] = (Uint8)(((Uint32)sp[0] * (Uint32)texture->color_r) / 255u);
                    dp[1] = (Uint8)(((Uint32)sp[1] * (Uint32)texture->color_g) / 255u);
                    dp[2] = (Uint8)(((Uint32)sp[2] * (Uint32)texture->color_b) / 255u);
                    if (texture->blend_mode == SDL_BLENDMODE_NONE){
                        dp[3] = 255u;
                    } else{
                        dp[3] = (Uint8)(((Uint32)sp[3] * (Uint32)texture->alpha_mod) / 255u);
                    }
                }
                if (qos_fb_blit_rgba((unsigned int)(dx + visible_from),
                                     (unsigned int)py,
                                     (unsigned int)out_n,
                                     1u,
                                     g_rowbuf) != 0){
                    set_error("fb blit failed");
                    return -1;
                }
            }
            run_start += run_len;
        }
    }
    return 0;
}

int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst){
    Uint64 t0 = qos_get_time_us();
    int rc = sdl_render_copy_internal(renderer, texture, src, dst, SDL_FLIP_NONE);
    g_sdl_profile.rendercopy_calls++;
    g_sdl_profile.rendercopy_us += qos_get_time_us() - t0;
    return rc;
}

int SDL_RenderCopyEx(SDL_Renderer* renderer,
                     SDL_Texture* texture,
                     const SDL_Rect* src,
                     const SDL_Rect* dst,
                     int angle_degrees,
                     const SDL_Point* center,
                     SDL_RendererFlip flip){
    (void)angle_degrees;
    (void)center;
    Uint64 t0 = qos_get_time_us();
    int rc = sdl_render_copy_internal(renderer, texture, src, dst, flip);
    g_sdl_profile.rendercopy_calls++;
    g_sdl_profile.rendercopy_us += qos_get_time_us() - t0;
    return rc;
}

int SDL_RenderTexture(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst){
    return SDL_RenderCopy(renderer, texture, src, dst);
}

int IMG_Init(int flags){
    (void)flags;
    return IMG_INIT_BMP;
}

void IMG_Quit(void){
}

SDL_Surface* IMG_Load(const char* file){
    return SDL_LoadBMP(file);
}

SDL_Texture* IMG_LoadTexture(SDL_Renderer* renderer, const char* file){
    SDL_Surface* s = SDL_LoadBMP(file);
    SDL_Texture* t;
    if (!s){
        return 0;
    }
    t = SDL_CreateTextureFromSurface(renderer, s);
    SDL_FreeSurface(s);
    return t;
}

const char* IMG_GetError(void){
    return SDL_GetError();
}
