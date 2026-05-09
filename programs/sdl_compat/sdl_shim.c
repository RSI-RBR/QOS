#include "SDL.h"
#include "syscall.h"

#define SDL_SHIM_MAX_TEXTURES 32
#define SDL_SHIM_TEX_POOL_BYTES (768u * 1024u)
#define SDL_SHIM_ROWBUF_PIXELS 2048

static SDL_Window g_window;
static SDL_Renderer g_renderer;
static SDL_Texture g_textures[SDL_SHIM_MAX_TEXTURES];
static Uint8 g_tex_pool[SDL_SHIM_TEX_POOL_BYTES];
static Uint32 g_tex_pool_used = 0u;
static Uint8 g_keyboard_state[256];
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static Uint32 g_mouse_buttons = 0u;
static char g_last_error[96] = "OK";
static Uint8 g_rowbuf[SDL_SHIM_ROWBUF_PIXELS * 4u];

static Uint32 rgb_to_color(Uint8 r, Uint8 g, Uint8 b){
    return ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
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

static int sdl_i_min(int a, int b){
    return (a < b) ? a : b;
}

static int sdl_i_max(int a, int b){
    return (a > b) ? a : b;
}

static int sdl_key_sym_from_qos(const qos_event_t* ev){
    if (!ev){
        return 0;
    }
    if (ev->ascii != 0u){
        return (int)ev->ascii;
    }
    if (ev->keycode == 0x29u){
        return SDLK_ESCAPE;
    }
    return (int)ev->keycode;
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

static void sdl_copy_bytes(Uint8* dst, const Uint8* src, Uint32 len){
    Uint32 i;
    if (!dst || !src){
        return;
    }
    for (i = 0u; i < len; i++){
        dst[i] = src[i];
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

static Uint8* tex_pool_alloc(Uint32 bytes){
    Uint32 aligned;
    if (bytes == 0u){
        return 0;
    }
    aligned = (bytes + 3u) & ~3u;
    if (aligned > SDL_SHIM_TEX_POOL_BYTES || g_tex_pool_used > SDL_SHIM_TEX_POOL_BYTES - aligned){
        return 0;
    }
    Uint8* out = &g_tex_pool[g_tex_pool_used];
    g_tex_pool_used += aligned;
    return out;
}

static int textures_alive_count(void){
    int alive = 0;
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        if (g_textures[i].alive){
            alive++;
        }
    }
    return alive;
}

static SDL_Texture* alloc_texture_slot(void){
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        if (!g_textures[i].alive){
            return &g_textures[i];
        }
    }
    return 0;
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
    for (int i = 0; i < 256; i++){
        g_keyboard_state[i] = 0u;
    }
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        g_textures[i].alive = 0;
        g_textures[i].pixels = 0;
        g_textures[i].capacity = 0u;
        g_textures[i].locked = 0;
    }
    g_tex_pool_used = 0u;
    g_window.alive = 0;
    g_renderer.alive = 0;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_buttons = 0u;
    set_error("OK");
    return 0;
}

int SDL_InitSubSystem(Uint32 flags){
    (void)flags;
    return 0;
}

void SDL_Quit(void){
    for (int i = 0; i < SDL_SHIM_MAX_TEXTURES; i++){
        g_textures[i].alive = 0;
        g_textures[i].pixels = 0;
        g_textures[i].capacity = 0u;
        g_textures[i].locked = 0;
    }
    g_tex_pool_used = 0u;
    g_renderer.alive = 0;
    g_window.alive = 0;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_buttons = 0u;
}

const char* SDL_GetError(void){
    return g_last_error;
}

Uint32 SDL_GetTicks(void){
    return (Uint32)(qos_get_time_us() / 1000ull);
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

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags){
    (void)index;
    (void)flags;
    if (!window || !window->alive){
        set_error("window not alive");
        return 0;
    }
    g_renderer.window = window;
    g_renderer.draw_color = 0x00000000u;
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
    qos_fb_rect(0u, 0u,
                (unsigned int)renderer->window->w,
                (unsigned int)renderer->window->h,
                renderer->draw_color);
    return 0;
}

int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect){
    int x, y, w, h;
    if (!renderer || !renderer->alive || !renderer->window || !rect){
        set_error("bad fill rect args");
        return -1;
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
    qos_event_t qos_ev;
    if (!event){
        return 0;
    }
    if (qos_poll_event(&qos_ev) <= 0){
        return 0;
    }

    if (qos_ev.type == QOS_EVENT_KEY_DOWN || qos_ev.type == QOS_EVENT_KEY_UP){
        int sym = sdl_key_sym_from_qos(&qos_ev);
        if (sym >= 0 && sym < 256){
            g_keyboard_state[sym] = (qos_ev.type == QOS_EVENT_KEY_DOWN) ? 1u : 0u;
        }

        event->type = (qos_ev.type == QOS_EVENT_KEY_DOWN) ? SDL_KEYDOWN : SDL_KEYUP;
        event->key.type = event->type;
        event->key.keysym.sym = sym;

        if (qos_ev.type == QOS_EVENT_KEY_DOWN &&
            (sym == 'q' || sym == 'Q' || sym == SDLK_ESCAPE)){
            event->type = SDL_QUIT;
            event->quit.type = SDL_QUIT;
        }
        return 1;
    }

    if (qos_ev.source == QOS_EVENT_SOURCE_MOUSE){
        g_mouse_x = qos_ev.x;
        g_mouse_y = qos_ev.y;
        g_mouse_buttons = sdl_mouse_mask_from_qos(qos_ev.buttons);
    }

    if (qos_ev.type == QOS_EVENT_MOUSE_MOVE){
        event->type = SDL_MOUSEMOTION;
        event->motion.type = SDL_MOUSEMOTION;
        event->motion.state = g_mouse_buttons;
        event->motion.x = qos_ev.x;
        event->motion.y = qos_ev.y;
        event->motion.xrel = qos_ev.dx;
        event->motion.yrel = qos_ev.dy;
        return 1;
    }

    if (qos_ev.type == QOS_EVENT_MOUSE_BUTTON_DOWN ||
        qos_ev.type == QOS_EVENT_MOUSE_BUTTON_UP){
        event->type = (qos_ev.type == QOS_EVENT_MOUSE_BUTTON_DOWN) ?
                      SDL_MOUSEBUTTONDOWN :
                      SDL_MOUSEBUTTONUP;
        event->button.type = event->type;
        event->button.button = sdl_button_from_qos(qos_ev.button);
        event->button.state = (qos_ev.type == QOS_EVENT_MOUSE_BUTTON_DOWN) ?
                              SDL_PRESSED :
                              SDL_RELEASED;
        event->button.clicks = 1u;
        event->button.padding1 = 0u;
        event->button.x = qos_ev.x;
        event->button.y = qos_ev.y;
        return 1;
    }

    if (qos_ev.type == QOS_EVENT_MOUSE_WHEEL){
        event->type = SDL_MOUSEWHEEL;
        event->wheel.type = SDL_MOUSEWHEEL;
        event->wheel.x = 0;
        event->wheel.y = qos_ev.wheel;
        event->wheel.mouse_x = qos_ev.x;
        event->wheel.mouse_y = qos_ev.y;
        return 1;
    }

    return 0;
}

const Uint8* SDL_GetKeyboardState(int* numkeys){
    if (numkeys){
        *numkeys = 256;
    }
    return g_keyboard_state;
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
    t->pixels = tex_pool_alloc(bytes);
    if (!t->pixels && textures_alive_count() == 0){
        g_tex_pool_used = 0u;
        t->pixels = tex_pool_alloc(bytes);
    }
    if (!t->pixels){
        set_error("texture pool exhausted");
        return 0;
    }

    t->w = w;
    t->h = h;
    t->format = format;
    t->access = (Uint32)access;
    t->pitch = w * 4;
    t->capacity = bytes;
    t->locked = 0;
    t->alive = 1;
    sdl_zero_bytes(t->pixels, bytes);
    return t;
}

void SDL_DestroyTexture(SDL_Texture* texture){
    if (!texture){
        return;
    }
    texture->alive = 0;
    texture->locked = 0;
    texture->pixels = 0;
    texture->capacity = 0u;
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
    return 0;
}

int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst){
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

    for (int oy = 0; oy < dh; oy++){
        int py = dy + oy;
        int tx_y = sy + (int)(((unsigned long long)oy * (unsigned long long)sh) / (unsigned long long)dh);
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
                    int tx_x = sx + (int)(((unsigned long long)ox * (unsigned long long)sw) / (unsigned long long)dw);
                    const Uint8* sp = texture->pixels +
                                      ((Uint32)tx_y * (Uint32)texture->pitch) +
                                      ((Uint32)tx_x * 4u);
                    Uint8* dp = &g_rowbuf[i * 4];
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                    dp[3] = sp[3];
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
