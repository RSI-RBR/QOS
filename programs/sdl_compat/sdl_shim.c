#include "SDL.h"
#include "SDL_image.h"
#include "syscall.h"
#include <stdlib.h>

#define SDL_SHIM_MAX_TEXTURES 128
#define SDL_SHIM_MAX_SURFACES 32
#define SDL_SHIM_ROWBUF_PIXELS 2048
#define SDL_SHIM_BMP_FILE_MAX (4u * 1024u * 1024u)
#define SDL_SHIM_EVENT_QUEUE_SIZE 64
#define SDL_SHIM_REPEAT_DELAY_MS 400u
#define SDL_SHIM_REPEAT_INTERVAL_MS 33u

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

static int sdl_i_min(int a, int b){
    return (a < b) ? a : b;
}

static int sdl_time_reached(Uint32 now, Uint32 target){
    return (int)(now - target) >= 0;
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
        g_textures[i].alive = 0;
        g_textures[i].pixels = 0;
        g_textures[i].capacity = 0u;
        g_textures[i].color_r = 255u;
        g_textures[i].color_g = 255u;
        g_textures[i].color_b = 255u;
        g_textures[i].alpha_mod = 255u;
        g_textures[i].blend_mode = SDL_BLENDMODE_BLEND;
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
        g_surfaces[i].color_key_enabled = 0;
        g_surfaces[i].owns_pixels = 0;
    }
    g_window.alive = 0;
    g_renderer.alive = 0;
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
        g_textures[i].alive = 0;
        g_textures[i].pixels = 0;
        g_textures[i].capacity = 0u;
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
        g_surfaces[i].owns_pixels = 0;
    }
    g_renderer.alive = 0;
    g_window.alive = 0;
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
    return &g_renderer;
}

void SDL_DestroyRenderer(SDL_Renderer* renderer){
    if (renderer == &g_renderer){
        g_renderer.alive = 0;
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

    if (renderer->draw_blend_mode == SDL_BLENDMODE_BLEND && renderer->draw_alpha == 0u){
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
    qos_event_t qos_ev;
    if (!event){
        return 0;
    }
    sdl_event_clear(event);

    if (sdl_event_pop(event)){
        return 1;
    }

    if (qos_poll_event(&qos_ev) <= 0){
        return sdl_poll_repeat_event(event);
    }

    if (sdl_translate_qos_event(&qos_ev, event)){
        sdl_queue_text_input_if_printable(&qos_ev);
        return 1;
    }
    return sdl_poll_repeat_event(event);
}

void SDL_PumpEvents(void){
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
    t->color_r = 255u;
    t->color_g = 255u;
    t->color_b = 255u;
    t->alpha_mod = 255u;
    t->blend_mode = SDL_BLENDMODE_BLEND;
    t->owns_pixels = 1;
    t->locked = 0;
    t->alive = 1;
    sdl_zero_bytes(t->pixels, bytes);
    return t;
}

SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface){
    SDL_Texture* t;
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

    t = alloc_texture_slot();
    if (!t){
        set_error("texture slots exhausted");
        return 0;
    }

    t->w = surface->w;
    t->h = surface->h;
    t->format = surface_format_value(surface);
    t->access = SDL_TEXTUREACCESS_STATIC;
    t->pitch = surface->pitch;
    t->capacity = surface->capacity;
    t->pixels = sdl_alloc_pixels(t->capacity);
    if (!t->pixels){
        set_error("texture heap exhausted");
        return 0;
    }
    sdl_copy_bytes(t->pixels, surface->pixels, t->capacity);
    t->color_r = 255u;
    t->color_g = 255u;
    t->color_b = 255u;
    t->alpha_mod = 255u;
    t->blend_mode = SDL_BLENDMODE_BLEND;
    t->owns_pixels = 1;
    t->locked = 0;
    t->alive = 1;
    return t;
}

void SDL_DestroyTexture(SDL_Texture* texture){
    if (!texture){
        return;
    }
    if (texture->alive && texture->owns_pixels){
        sdl_free_pixels(texture->pixels);
    }
    texture->alive = 0;
    texture->locked = 0;
    texture->pixels = 0;
    texture->capacity = 0u;
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
    Uint8* bmp = 0;

    if (!file || !*file){
        set_error("bad BMP filename");
        return 0;
    }

    bmp = (Uint8*)malloc(SDL_SHIM_BMP_FILE_MAX);
    if (!bmp){
        set_error("BMP temp heap exhausted");
        return 0;
    }

    n = qos_file_read_bmp(file, bmp, SDL_SHIM_BMP_FILE_MAX);
    if (n < 54){
        free(bmp);
        set_error("BMP read failed");
        return 0;
    }
    if (bmp[0] != 'B' || bmp[1] != 'M'){
        free(bmp);
        set_error("not a BMP");
        return 0;
    }

    pixel_offset = read_le32(&bmp[10]);
    dib_size = read_le32(&bmp[14]);
    if (dib_size < 40u || pixel_offset >= (Uint32)n){
        free(bmp);
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
        set_error("BMP dimensions unsupported");
        return 0;
    }

    row_stride = ((((Uint32)width * (Uint32)bpp) + 31u) / 32u) * 4u;
    if (row_stride == 0u ||
        (Uint32)height > (0xFFFFFFFFu - pixel_offset) / row_stride){
        free(bmp);
        set_error("BMP row overflow");
        return 0;
    }
    if (pixel_offset + ((Uint32)height * row_stride) > (Uint32)n){
        free(bmp);
        set_error("BMP truncated");
        return 0;
    }
    if ((Uint32)width > (0xFFFFFFFFu / (Uint32)height) / 4u){
        free(bmp);
        set_error("BMP size overflow");
        return 0;
    }
    pixel_bytes = (Uint32)width * (Uint32)height * 4u;

    s = alloc_surface_slot();
    if (!s){
        free(bmp);
        set_error("surface slots exhausted");
        return 0;
    }
    s->pixels = sdl_alloc_pixels(pixel_bytes);
    if (!s->pixels){
        free(bmp);
        set_error("surface heap exhausted");
        return 0;
    }

    s->w = width;
    s->h = height;
    s->pitch = width * 4;
    surface_set_format(s, SDL_PIXELFORMAT_RGBA8888);
    s->capacity = pixel_bytes;
    s->color_key = 0u;
    s->color_key_enabled = 0;
    s->owns_pixels = 1;
    s->alive = 1;

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
            if (dp[3] != 0u){
                alpha_nonzero = 1;
            }
        }
    }

    if (bpp == 32u && !alpha_nonzero){
        for (Uint32 i = 0u; i < pixel_bytes; i += 4u){
            s->pixels[i + 3u] = 255u;
        }
    }

    free(bmp);
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
    s->color_key_enabled = 0;
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
    return 0;
}

static void apply_surface_color_key(SDL_Surface* surface){
    if (!surface || !surface->alive || !surface->pixels || !surface->color_key_enabled){
        return;
    }

    Uint8 kr = color_r(surface->color_key);
    Uint8 kg = color_g(surface->color_key);
    Uint8 kb = color_b(surface->color_key);
    for (int y = 0; y < surface->h; y++){
        Uint8* row = surface->pixels + ((Uint32)y * (Uint32)surface->pitch);
        for (int x = 0; x < surface->w; x++){
            Uint8* p = row + ((Uint32)x * 4u);
            if (p[0] == kr && p[1] == kg && p[2] == kb){
                p[3] = 0u;
            }
        }
    }
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
    return sdl_render_copy_internal(renderer, texture, src, dst, SDL_FLIP_NONE);
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
    return sdl_render_copy_internal(renderer, texture, src, dst, flip);
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
