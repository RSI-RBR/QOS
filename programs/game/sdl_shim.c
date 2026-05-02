#include "SDL.h"
#include "syscall.h"

static SDL_Window g_window;
static SDL_Renderer g_renderer;
static SDL_Texture g_texture;
static Uint8 g_keyboard_state[256];
static char g_last_error[64] = "OK";

static Uint32 rgb_to_color(Uint8 r, Uint8 g, Uint8 b){
    return ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

static void set_error(const char* msg){
    int i = 0;
    if (!msg){
        g_last_error[0] = 0;
        return;
    }
    while (msg[i] && i < (int)(sizeof(g_last_error) - 1)){
        g_last_error[i] = msg[i];
        i++;
    }
    g_last_error[i] = 0;
}

int SDL_Init(Uint32 flags){
    (void)flags;
    for (int i = 0; i < 256; i++){
        g_keyboard_state[i] = 0;
    }
    g_window.alive = 0;
    g_renderer.alive = 0;
    g_texture.alive = 0;
    set_error("OK");
    return 0;
}

int SDL_InitSubSystem(Uint32 flags){
    (void)flags;
    return 0;
}

void SDL_Quit(void){
    g_texture.alive = 0;
    g_renderer.alive = 0;
    g_window.alive = 0;
}

const char* SDL_GetError(void){
    return g_last_error;
}

Uint32 SDL_GetTicks(void){
    return (Uint32)qos_get_ticks();
}

void SDL_Delay(Uint32 ms){
    qos_sleep(ms);
}

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags){
    (void)title;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    g_window.w = (int)qos_get_screen_width();
    g_window.h = (int)qos_get_screen_height();
    g_window.flags = flags | SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_window.alive = 1;
    return &g_window;
}

void SDL_DestroyWindow(SDL_Window* window){
    if (window == &g_window){
        g_window.alive = 0;
    }
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags){
    (void)index;
    (void)flags;
    if (!window || !window->alive){
        set_error("window not alive");
        return 0;
    }
    g_renderer.window = window;
    g_renderer.draw_color = 0x000000;
    g_renderer.alive = 1;
    return &g_renderer;
}

void SDL_DestroyRenderer(SDL_Renderer* renderer){
    if (renderer == &g_renderer){
        g_renderer.alive = 0;
    }
}

int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a){
    (void)a;
    if (!renderer || !renderer->alive){
        set_error("renderer not alive");
        return -1;
    }
    renderer->draw_color = rgb_to_color(r, g, b);
    return 0;
}

int SDL_RenderClear(SDL_Renderer* renderer){
    if (!renderer || !renderer->alive || !renderer->window){
        set_error("renderer/window not alive");
        return -1;
    }
    qos_fb_rect(0, 0,
                (unsigned int)renderer->window->w,
                (unsigned int)renderer->window->h,
                renderer->draw_color);
    return 0;
}

int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect){
    if (!renderer || !renderer->alive || !renderer->window || !rect){
        set_error("bad fill rect args");
        return -1;
    }
    if (rect->w <= 0 || rect->h <= 0){
        return 0;
    }
    if (rect->x >= renderer->window->w || rect->y >= renderer->window->h){
        return 0;
    }

    int x = rect->x < 0 ? 0 : rect->x;
    int y = rect->y < 0 ? 0 : rect->y;
    int w = rect->w;
    int h = rect->h;
    if (x + w > renderer->window->w){
        w = renderer->window->w - x;
    }
    if (y + h > renderer->window->h){
        h = renderer->window->h - y;
    }
    if (w <= 0 || h <= 0){
        return 0;
    }

    qos_fb_rect((unsigned int)x, (unsigned int)y, (unsigned int)w, (unsigned int)h, renderer->draw_color);
    return 0;
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
    (void)renderer;
    qos_fb_present();
}

int SDL_PollEvent(SDL_Event* event){
    int ch;
    if (!event){
        return 0;
    }
    ch = qos_try_getc();
    if (ch < 0){
        return 0;
    }

    if (ch >= 0 && ch < 256){
        g_keyboard_state[ch] = 1;
    }

    event->type = SDL_KEYDOWN;
    event->key.type = SDL_KEYDOWN;
    event->key.keysym.sym = ch;

    if (ch == 'q' || ch == 'Q' || ch == SDLK_ESCAPE){
        event->type = SDL_QUIT;
        event->quit.type = SDL_QUIT;
    }
    return 1;
}

const Uint8* SDL_GetKeyboardState(int* numkeys){
    if (numkeys){
        *numkeys = 256;
    }
    return g_keyboard_state;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h){
    (void)renderer;
    (void)access;
    g_texture.w = w;
    g_texture.h = h;
    g_texture.format = format;
    g_texture.alive = 1;
    return &g_texture;
}

void SDL_DestroyTexture(SDL_Texture* texture){
    if (texture == &g_texture){
        g_texture.alive = 0;
    }
}

int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst){
    (void)renderer;
    (void)texture;
    (void)src;
    (void)dst;
    set_error("SDL_RenderCopy not implemented in shim");
    return -1;
}
