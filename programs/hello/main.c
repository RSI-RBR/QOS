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
    unsigned long accum_update_ms = 0;
    unsigned long accum_draw_ms = 0;
    unsigned long accum_present_ms = 0;
    unsigned long accum_total_ms = 0;
    unsigned long max_frame_ms = 0;
    const unsigned long report_every = 120;
    int reporter = (pid == 1);

    while (running){
        unsigned long t0 = qos_get_ticks();
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
        unsigned long t1 = qos_get_ticks();

        qos_fb_rect(cube.lx, cube.ly, cube.sx, cube.sy, cube.c);
        unsigned long t2 = qos_get_ticks();

        qos_fb_present();
        unsigned long t3 = qos_get_ticks();

        qos_sleep(16);
        unsigned long t4 = qos_get_ticks();

        unsigned long update_ms = t1 - t0;
        unsigned long draw_ms = t2 - t1;
        unsigned long present_ms = t3 - t2;
        unsigned long total_ms = t4 - t0;

        accum_update_ms += update_ms;
        accum_draw_ms += draw_ms;
        accum_present_ms += present_ms;
        accum_total_ms += total_ms;
        if (total_ms > max_frame_ms){
            max_frame_ms = total_ms;
        }
        frame_count++;

        if (reporter && (frame_count % report_every) == 0){
            unsigned long now = qos_get_ticks();
            unsigned long window_ms = now - report_start_ticks;
            if (window_ms == 0){
                window_ms = 1;
            }
            unsigned long fps_x10 = (report_every * 10000UL) / window_ms;

            qos_puts("HELLO PERF pid=");
            print_u32((unsigned long)pid);
            qos_puts(" fps=");
            print_u32(fps_x10 / 10UL);
            qos_putc('.');
            print_u32(fps_x10 % 10UL);
            qos_puts(" upd=");
            print_u32(accum_update_ms / report_every);
            qos_puts(" draw=");
            print_u32(accum_draw_ms / report_every);
            qos_puts(" present=");
            print_u32(accum_present_ms / report_every);
            qos_puts(" frame=");
            print_u32(accum_total_ms / report_every);
            qos_puts(" max=");
            print_u32(max_frame_ms);
            qos_puts(" ms\n");

            report_start_ticks = now;
            accum_update_ms = 0;
            accum_draw_ms = 0;
            accum_present_ms = 0;
            accum_total_ms = 0;
            max_frame_ms = 0;
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
