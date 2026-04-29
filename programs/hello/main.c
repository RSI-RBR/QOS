#include "api.h"

struct screen_saver_cube{
    int lx;
    int ly;
    int sx;
    int sy;
    int vx;
    int vy;
    unsigned int c;
};

void program_main(kernel_api_t *api){
    api->puts("Hello from external program!\n");

    int screen_x = 1920; int screen_y = 1080;
//    api->draw_rect(0, 0, 50, 50, 0x00FF0000);
    char running = 1;
    struct screen_saver_cube cube; cube.lx = 0; cube.ly = 0; cube.vx = 1; cube.vy = 1; cube.c = 0x00FFFFFF; cube.sx = 50; cube.sy = 50;
    while (running){
//        api->clear(0x00000000);
        api->draw_rect(cube.lx, cube.ly, cube.sx, cube.sy, 0x00000000);
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

        api->draw_rect(cube.lx, cube.ly, cube.sx, cube.sy, cube.c);
        api->sleep(16);
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
