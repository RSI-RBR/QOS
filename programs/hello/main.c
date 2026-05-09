#include "syscall.h"

struct screen_saver_cube{
    int lx;
    int ly;
    int sx;
    int sy;
    int vx;
    int vy;
    unsigned int c;
};

static void print_u32(unsigned long v){
    char tmp[21];
    int n = 0;
    if (v == 0){
        qos_putc('0');
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0){
        qos_putc(tmp[--n]);
    }
}

static unsigned long cycles_to_us(unsigned long cycles, unsigned long hz){
    if (hz == 0){
        return 0;
    }
    return (cycles * 1000000UL) / hz;
}

static void print_ms_3(unsigned long us){
    unsigned long ms_whole = us / 1000UL;
    unsigned long ms_frac = us % 1000UL;
    print_u32(ms_whole);
    qos_putc('.');
    if (ms_frac < 100UL){
        qos_putc('0');
    }
    if (ms_frac < 10UL){
        qos_putc('0');
    }
    print_u32(ms_frac);
}

static unsigned char hud_glyph_row(char c, unsigned int row){
    if (row >= 5u){
        return 0;
    }

    switch (c){
        case '0': { static const unsigned char r[5] = {7, 5, 5, 5, 7}; return r[row]; }
        case '1': { static const unsigned char r[5] = {2, 6, 2, 2, 7}; return r[row]; }
        case '2': { static const unsigned char r[5] = {7, 1, 7, 4, 7}; return r[row]; }
        case '3': { static const unsigned char r[5] = {7, 1, 7, 1, 7}; return r[row]; }
        case '4': { static const unsigned char r[5] = {5, 5, 7, 1, 1}; return r[row]; }
        case '5': { static const unsigned char r[5] = {7, 4, 7, 1, 7}; return r[row]; }
        case '6': { static const unsigned char r[5] = {7, 4, 7, 5, 7}; return r[row]; }
        case '7': { static const unsigned char r[5] = {7, 1, 1, 2, 2}; return r[row]; }
        case '8': { static const unsigned char r[5] = {7, 5, 7, 5, 7}; return r[row]; }
        case '9': { static const unsigned char r[5] = {7, 5, 7, 1, 7}; return r[row]; }
        case 'F': { static const unsigned char r[5] = {7, 4, 6, 4, 4}; return r[row]; }
        case 'P': { static const unsigned char r[5] = {6, 5, 6, 4, 4}; return r[row]; }
        case 'S': { static const unsigned char r[5] = {7, 4, 7, 1, 7}; return r[row]; }
        case 'M': { static const unsigned char r[5] = {5, 7, 7, 5, 5}; return r[row]; }
        case 'O': { static const unsigned char r[5] = {7, 5, 5, 5, 7}; return r[row]; }
        case 'U': { static const unsigned char r[5] = {5, 5, 5, 5, 7}; return r[row]; }
        case 'E': { static const unsigned char r[5] = {7, 4, 6, 4, 7}; return r[row]; }
        case ':': { static const unsigned char r[5] = {0, 2, 0, 2, 0}; return r[row]; }
        case '.': { static const unsigned char r[5] = {0, 0, 0, 0, 2}; return r[row]; }
        default:
            return 0;
    }
}

#define HUD_W 300u
#define HUD_H 32u

static unsigned char g_hud_rgba[HUD_W * HUD_H * 4u] = {1u};

static void hud_put_pixel(unsigned int x,
                          unsigned int y,
                          unsigned int r,
                          unsigned int g,
                          unsigned int b,
                          unsigned int a){
    if (x >= HUD_W || y >= HUD_H){
        return;
    }
    unsigned int i = ((y * HUD_W) + x) * 4u;
    g_hud_rgba[i + 0u] = (unsigned char)r;
    g_hud_rgba[i + 1u] = (unsigned char)g;
    g_hud_rgba[i + 2u] = (unsigned char)b;
    g_hud_rgba[i + 3u] = (unsigned char)a;
}

static void hud_fill_rect(unsigned int x,
                          unsigned int y,
                          unsigned int w,
                          unsigned int h,
                          unsigned int r,
                          unsigned int g,
                          unsigned int b,
                          unsigned int a){
    for (unsigned int py = 0; py < h; py++){
        for (unsigned int px = 0; px < w; px++){
            hud_put_pixel(x + px, y + py, r, g, b, a);
        }
    }
}

static void hud_clear_buffer(void){
    for (unsigned int i = 0; i < sizeof(g_hud_rgba); i += 4u){
        g_hud_rgba[i + 0u] = 0u;
        g_hud_rgba[i + 1u] = 0u;
        g_hud_rgba[i + 2u] = 0u;
        g_hud_rgba[i + 3u] = 255u;
    }
}

static unsigned int hud_draw_char_to_buffer(unsigned int x,
                                            unsigned int y,
                                            char c,
                                            unsigned int scale){
    if (c == ' '){
        return 4u * scale;
    }
    for (unsigned int row = 0; row < 5u; row++){
        unsigned char bits = hud_glyph_row(c, row);
        for (unsigned int col = 0; col < 3u; col++){
            if (bits & (1u << (2u - col))){
                hud_fill_rect(x + (col * scale),
                              y + (row * scale),
                              scale,
                              scale,
                              0u,
                              255u,
                              102u,
                              255u);
            }
        }
    }
    return 4u * scale;
}

static unsigned int hud_draw_text(unsigned int x,
                                  unsigned int y,
                                  const char* s,
                                  unsigned int scale){
    while (*s){
        x += hud_draw_char_to_buffer(x, y, *s, scale);
        s++;
    }
    return x;
}

static unsigned int hud_draw_u32(unsigned int x,
                                 unsigned int y,
                                 unsigned long v,
                                 unsigned int scale){
    char tmp[16];
    int n = 0;
    if (v == 0){
        return x + hud_draw_char_to_buffer(x, y, '0', scale);
    }
    while (v > 0 && n < (int)sizeof(tmp)){
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0){
        x += hud_draw_char_to_buffer(x, y, tmp[--n], scale);
    }
    return x;
}

static unsigned int hud_draw_fixed_1(unsigned int x,
                                     unsigned int y,
                                     unsigned long value_x10,
                                     unsigned int scale){
    x = hud_draw_u32(x, y, value_x10 / 10UL, scale);
    x += hud_draw_char_to_buffer(x, y, '.', scale);
    x = hud_draw_u32(x, y, value_x10 % 10UL, scale);
    return x;
}

static void hud_draw(unsigned long fps_x10, unsigned long mouse_hz){
    const unsigned int scale = 4u;
    const unsigned int x = 4u;
    const unsigned int y = 4u;
    unsigned int cx;

    hud_clear_buffer();
    cx = hud_draw_text(x, y, "FPS:", scale);
    cx = hud_draw_fixed_1(cx, y, fps_x10, scale);
    cx = hud_draw_text(cx + (2u * scale), y, "MOUSE:", scale);
    (void)hud_draw_u32(cx, y, mouse_hz, scale);
    (void)qos_fb_blit_rgba(4u, 4u, HUD_W, HUD_H, g_hud_rgba);
}

void program_main(void){
//    qos_puts("Hello from external program!\n");

    int screen_x = (int)qos_get_screen_width();
    int screen_y = (int)qos_get_screen_height();
    int pid = qos_getpid();
    if (pid < 0){
        pid = 0;
    }
    if (screen_x <= 0) screen_x = 1920;
    if (screen_y <= 0) screen_y = 1080;
//    api->draw_rect(0, 0, 50, 50, 0x00FF0000);
    char running = 1;
    struct screen_saver_cube cube;
    cube.sx = 50;
    cube.sy = 50;
    cube.lx = (pid * 73) % (screen_x > cube.sx ? (screen_x - cube.sx) : 1);
    cube.ly = (pid * 47) % (screen_y > cube.sy ? (screen_y - cube.sy) : 1);
    cube.vx = (pid & 1) ? 1 : -1;
    cube.vy = (pid & 2) ? 1 : -1;
    cube.c = 0x00FFFFFF;

    unsigned long frame_count = 0;
    unsigned long report_start_ticks = qos_get_ticks();
    unsigned long counter_hz = qos_get_counter_hz();
    if (counter_hz == 0){
        counter_hz = 1;
    }
    unsigned long accum_update_us = 0;
    unsigned long accum_draw_us = 0;
    unsigned long accum_present_us = 0;
    unsigned long accum_total_us = 0;
    unsigned long max_frame_us = 0;
    const unsigned long report_every = 120;
    int reporter = (pid == 1);
    unsigned long hud_start_us = qos_get_time_us();
    unsigned long hud_frames = 0;
    unsigned long hud_mouse_moves = 0;
    unsigned long hud_fps_x10 = 0;
    unsigned long hud_mouse_hz = 0;
    int hud_dirty = 1;
    const unsigned long long target_frame_us = 16667ull;

    while (running){
        unsigned long long frame_start_us = qos_get_time_us();
        unsigned long t0 = qos_get_counter_cycles();
        qos_event_t ev;
        while (qos_poll_event(&ev) > 0){
            if (ev.type == QOS_EVENT_MOUSE_MOVE){
                hud_mouse_moves++;
            }
        }
//        api->clear(0x00000000);
        qos_fb_rect(cube.lx, cube.ly, cube.sx, cube.sy, 0x00000000);
        int nx = cube.lx + cube.vx;
        int ny = cube.ly + cube.vy;

        if (nx < 0){
            nx = 0;
            cube.vx *= -1;
        }else if (nx + cube.sx > screen_x){
            nx = screen_x - cube.sx;
            cube.vx *= -1;
        }

        if (ny < 0){
            ny = 0;
            cube.vy *= -1;
        } else if (ny + cube.sy > screen_y){
            ny = screen_y - cube.sy;
            cube.vy *= -1;
        }

        cube.lx = nx; cube.ly = ny;
        unsigned long t1 = qos_get_counter_cycles();

        qos_fb_rect(cube.lx, cube.ly, cube.sx, cube.sy, cube.c);
        unsigned long t2 = qos_get_counter_cycles();

        hud_frames++;
        unsigned long hud_now_us = qos_get_time_us();
        unsigned long hud_window_us = hud_now_us - hud_start_us;
        if (hud_window_us >= 500000UL){
            hud_fps_x10 = (hud_frames * 10000000UL) / hud_window_us;
            hud_mouse_hz = (hud_mouse_moves * 1000000UL) / hud_window_us;
            hud_frames = 0;
            hud_mouse_moves = 0;
            hud_start_us = hud_now_us;
            hud_dirty = 1;
        }
        if (hud_dirty){
            hud_draw(hud_fps_x10, hud_mouse_hz);
            hud_dirty = 0;
        }

        qos_fb_present();
        unsigned long t3 = qos_get_counter_cycles();

        unsigned long long frame_elapsed_us = qos_get_time_us() - frame_start_us;
        if (frame_elapsed_us < target_frame_us){
            qos_sleep_us(target_frame_us - frame_elapsed_us);
        }
        unsigned long t4 = qos_get_counter_cycles();

        unsigned long update_us = cycles_to_us(t1 - t0, counter_hz);
        unsigned long draw_us = cycles_to_us(t2 - t1, counter_hz);
        unsigned long present_us = cycles_to_us(t3 - t2, counter_hz);
        unsigned long total_us = cycles_to_us(t4 - t0, counter_hz);

        accum_update_us += update_us;
        accum_draw_us += draw_us;
        accum_present_us += present_us;
        accum_total_us += total_us;
        if (total_us > max_frame_us){
            max_frame_us = total_us;
        }
        frame_count++;

        if (reporter && (frame_count % report_every) == 0){
            unsigned long now = qos_get_ticks();
            unsigned long window_ms = now - report_start_ticks;
            if (window_ms == 0){
                window_ms = 1;
            }
            unsigned long fps_x10 = (report_every * 10000UL) / window_ms;

//            qos_puts("HELLO PERF pid=");
//            print_u32((unsigned long)pid);
//            qos_puts(" fps=");
//            print_u32(fps_x10 / 10UL);
//            qos_putc('.');
//            print_u32(fps_x10 % 10UL);
//            qos_puts(" upd=");
//            print_ms_3(accum_update_us / report_every);
//            qos_puts(" draw=");
//            print_ms_3(accum_draw_us / report_every);
//            qos_puts(" present=");
//            print_ms_3(accum_present_us / report_every);
//            qos_puts(" frame=");
//            print_ms_3(accum_total_us / report_every);
//            qos_puts(" max=");
//            print_ms_3(max_frame_us);
//            qos_puts(" ms\n");

            report_start_ticks = now;
            accum_update_us = 0;
            accum_draw_us = 0;
            accum_present_us = 0;
            accum_total_us = 0;
            max_frame_us = 0;
        }
    }
    

//    void *mem = api->malloc(256);
//
//    if (mem){
//        api->puts("Allocated memory\n");
//        api->free(mem);
//    }
//    api->puts("Program main returning...\n");
    return;
}
