#include "SDL.h"
#include "syscall.h"

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

int program_main(void){
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Event e;
    SDL_Rect r;
    int running = 1;
    int vx = 2;
    int vy = 2;
    unsigned long frames = 0;
    unsigned long t0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0){
        qos_puts("game scaffold: SDL_Init failed\n");
        return 1;
    }

    window = SDL_CreateWindow("QOS Game", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window){
        qos_puts("game scaffold: window create failed\n");
        SDL_Quit();
        return 1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer){
        qos_puts("game scaffold: renderer create failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    r.x = window->w / 3;
    r.y = window->h / 3;
    r.w = 64;
    r.h = 64;

    t0 = SDL_GetTicks();
    qos_puts("game scaffold running; press q to quit\n");

    while (running){
        while (SDL_PollEvent(&e)){
            if (e.type == SDL_QUIT){
                running = 0;
            }
        }

        r.x += vx;
        r.y += vy;
        if (r.x < 0){
            r.x = 0;
            vx = -vx;
        } else if (r.x + r.w > window->w){
            r.x = window->w - r.w;
            vx = -vx;
        }
        if (r.y < 0){
            r.y = 0;
            vy = -vy;
        } else if (r.y + r.h > window->h){
            r.y = window->h - r.h;
            vy = -vy;
        }

        (void)SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        (void)SDL_RenderClear(renderer);
        (void)SDL_SetRenderDrawColor(renderer, 0, 220, 120, 255);
        (void)SDL_RenderFillRect(renderer, &r);
        SDL_RenderPresent(renderer);

        frames++;
        if ((frames % 120UL) == 0UL){
            unsigned long now = SDL_GetTicks();
            unsigned long dt = now - t0;
            if (dt == 0){
                dt = 1;
            }
            unsigned long fps_x10 = (120UL * 10000UL) / dt;
            qos_puts("GAME PERF fps=");
            print_u32(fps_x10 / 10UL);
            qos_putc('.');
            print_u32(fps_x10 % 10UL);
            qos_puts("\n");
            t0 = now;
        }

        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
