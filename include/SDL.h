#ifndef QOS_SDL_H
#define QOS_SDL_H

#ifdef QOS_USERSPACE
#include "syscall.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char Uint8;
typedef unsigned short Uint16;
typedef unsigned int Uint32;
typedef unsigned long long Uint64;
typedef signed int Sint32;
typedef int SDL_bool;
typedef int SDL_Scancode;
typedef int SDL_Keycode;

#define SDL_FALSE 0
#define SDL_TRUE 1
#define SDL_DISABLE 0
#define SDL_ENABLE 1

typedef enum SDL_BlendMode {
    SDL_BLENDMODE_NONE = 0,
    SDL_BLENDMODE_BLEND = 1,
    SDL_BLENDMODE_ADD = 2,
    SDL_BLENDMODE_MOD = 4
} SDL_BlendMode;

typedef struct SDL_Window {
    int w;
    int h;
    Uint32 flags;
    int alive;
} SDL_Window;

typedef struct SDL_Renderer {
    SDL_Window* window;
    Uint32 draw_color;
    Uint8 draw_alpha;
    SDL_BlendMode draw_blend_mode;
    int alive;
} SDL_Renderer;

typedef struct SDL_Texture {
    int w;
    int h;
    Uint32 format;
    Uint32 access;
    int pitch;
    Uint8* pixels;
    Uint32 capacity;
    Uint8 color_r;
    Uint8 color_g;
    Uint8 color_b;
    Uint8 alpha_mod;
    SDL_BlendMode blend_mode;
    Uint32 average_color;
    int opaque;
    int render_hint;
    int owns_pixels;
    int locked;
    int alive;
} SDL_Texture;

typedef struct SDL_PixelFormat {
    Uint32 format;
    Uint8 bytes_per_pixel;
} SDL_PixelFormat;

typedef struct SDL_Color {
    Uint8 r;
    Uint8 g;
    Uint8 b;
    Uint8 a;
} SDL_Color;

typedef struct SDL_Surface {
    int w;
    int h;
    int pitch;
    SDL_PixelFormat* format;
    SDL_PixelFormat format_storage;
    Uint8* pixels;
    Uint32 capacity;
    Uint32 color_key;
    Uint32 average_color;
    int color_key_enabled;
    int opaque;
    int render_hint;
    int owns_pixels;
    int alive;
} SDL_Surface;

typedef struct SDL_Rect {
    int x;
    int y;
    int w;
    int h;
} SDL_Rect;

typedef struct SDL_Point {
    int x;
    int y;
} SDL_Point;

typedef struct SDL_Keysym {
    SDL_Scancode scancode;
    SDL_Keycode sym;
    Uint16 mod;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint8 state;
    Uint8 repeat;
    Uint8 padding2;
    Uint8 padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_MouseMotionEvent {
    Uint32 type;
    Uint32 state;
    int x;
    int y;
    int xrel;
    int yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent {
    Uint32 type;
    Uint8 button;
    Uint8 state;
    Uint8 clicks;
    Uint8 padding1;
    int x;
    int y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent {
    Uint32 type;
    int x;
    int y;
    int mouse_x;
    int mouse_y;
} SDL_MouseWheelEvent;

typedef struct SDL_QuitEvent {
    Uint32 type;
} SDL_QuitEvent;

typedef struct SDL_TextInputEvent {
    Uint32 type;
    char text[32];
} SDL_TextInputEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_QuitEvent quit;
    SDL_TextInputEvent text;
} SDL_Event;

typedef struct SDL_DisplayMode {
    Uint32 format;
    int w;
    int h;
    int refresh_rate;
    void* driverdata;
} SDL_DisplayMode;

#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_EVERYTHING (SDL_INIT_TIMER | SDL_INIT_VIDEO)

#define SDL_WINDOW_FULLSCREEN 0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOW_BORDERLESS 0x00000010u
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u

#define SDL_RENDERER_SOFTWARE 0x00000001u
#define SDL_RENDERER_ACCELERATED 0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u
#define SDL_PIXELFORMAT_RGBA8888 0x16462004u
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET 2

#define SDL_QUIT 0x100u
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDL_MOUSEWHEEL 0x403u
#define SDL_TEXTINPUT 0x303u

#define SDL_EVENT_QUIT SDL_QUIT
#define SDL_EVENT_KEY_DOWN SDL_KEYDOWN
#define SDL_EVENT_KEY_UP SDL_KEYUP
#define SDL_EVENT_MOUSE_MOTION SDL_MOUSEMOTION
#define SDL_EVENT_MOUSE_BUTTON_DOWN SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_MOUSE_BUTTON_UP SDL_MOUSEBUTTONUP
#define SDL_EVENT_MOUSE_WHEEL SDL_MOUSEWHEEL
#define SDL_EVENT_TEXT_INPUT SDL_TEXTINPUT

#define SDL_PRESSED 1u
#define SDL_RELEASED 0u

#define SDL_BUTTON_LEFT 1u
#define SDL_BUTTON_MIDDLE 2u
#define SDL_BUTTON_RIGHT 3u

#define SDL_BUTTON(X) (1u << ((X) - 1u))
#define SDL_BUTTON_LMASK 1u
#define SDL_BUTTON_MMASK 2u
#define SDL_BUTTON_RMASK 4u

#define KMOD_NONE  0x0000u
#define KMOD_SHIFT 0x0001u
#define KMOD_CTRL  0x0002u
#define KMOD_ALT   0x0004u
#define KMOD_GUI   0x0008u

#define SDL_DEFAULT_REPEAT_DELAY 400
#define SDL_DEFAULT_REPEAT_INTERVAL 33
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_HINT_RENDER_SCALE_QUALITY"
#define SDL_TICKS_PASSED(A, B) ((Sint32)((B) - (A)) <= 0)

#define SDL_SCANCODE_UNKNOWN 0
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_B 5
#define SDL_SCANCODE_C 6
#define SDL_SCANCODE_D 7
#define SDL_SCANCODE_E 8
#define SDL_SCANCODE_F 9
#define SDL_SCANCODE_G 10
#define SDL_SCANCODE_H 11
#define SDL_SCANCODE_I 12
#define SDL_SCANCODE_J 13
#define SDL_SCANCODE_K 14
#define SDL_SCANCODE_L 15
#define SDL_SCANCODE_M 16
#define SDL_SCANCODE_N 17
#define SDL_SCANCODE_O 18
#define SDL_SCANCODE_P 19
#define SDL_SCANCODE_Q 20
#define SDL_SCANCODE_R 21
#define SDL_SCANCODE_S 22
#define SDL_SCANCODE_T 23
#define SDL_SCANCODE_U 24
#define SDL_SCANCODE_V 25
#define SDL_SCANCODE_W 26
#define SDL_SCANCODE_X 27
#define SDL_SCANCODE_Y 28
#define SDL_SCANCODE_Z 29
#define SDL_SCANCODE_1 30
#define SDL_SCANCODE_2 31
#define SDL_SCANCODE_3 32
#define SDL_SCANCODE_4 33
#define SDL_SCANCODE_5 34
#define SDL_SCANCODE_6 35
#define SDL_SCANCODE_7 36
#define SDL_SCANCODE_8 37
#define SDL_SCANCODE_9 38
#define SDL_SCANCODE_0 39
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_ESCAPE 41
#define SDL_SCANCODE_BACKSPACE 42
#define SDL_SCANCODE_TAB 43
#define SDL_SCANCODE_SPACE 44
#define SDL_SCANCODE_RIGHT 79
#define SDL_SCANCODE_LEFT 80
#define SDL_SCANCODE_DOWN 81
#define SDL_SCANCODE_UP 82
#define SDL_SCANCODE_LCTRL 224
#define SDL_SCANCODE_LSHIFT 225
#define SDL_SCANCODE_LALT 226
#define SDL_SCANCODE_LGUI 227
#define SDL_SCANCODE_RCTRL 228
#define SDL_SCANCODE_RSHIFT 229
#define SDL_SCANCODE_RALT 230
#define SDL_SCANCODE_RGUI 231
#define SDL_NUM_SCANCODES 512

#define SDL_FLIP_NONE 0
#define SDL_FLIP_HORIZONTAL 1
#define SDL_FLIP_VERTICAL 2
typedef int SDL_RendererFlip;

#define SDLK_RETURN '\r'
#define SDLK_ESCAPE 27
#define SDLK_BACKSPACE '\b'
#define SDLK_TAB '\t'
#define SDLK_SPACE ' '
#define SDLK_RIGHT 1073741903
#define SDLK_LEFT 1073741904
#define SDLK_DOWN 1073741905
#define SDLK_UP 1073741906
#define SDLK_LCTRL 1073742048
#define SDLK_LSHIFT 1073742049
#define SDLK_LALT 1073742050
#define SDLK_LGUI 1073742051
#define SDLK_RCTRL 1073742052
#define SDLK_RSHIFT 1073742053
#define SDLK_RALT 1073742054
#define SDLK_RGUI 1073742055

int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_Quit(void);

const char* SDL_GetError(void);
char* SDL_GetBasePath(void);
void SDL_free(void* mem);
int SDL_SetHint(const char* name, const char* value);

Uint32 SDL_GetTicks(void);
Uint64 SDL_GetTicks64(void);
void SDL_Delay(Uint32 ms);
#ifndef QOS_USERSPACE
Uint64 SDL_GetTicksNS(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
#endif

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window* window);
int SDL_GetCurrentDisplayMode(int index, SDL_DisplayMode* mode);

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer* renderer);

int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_SetRenderDrawBlendMode(SDL_Renderer* renderer, SDL_BlendMode blend_mode);
int SDL_GetRenderDrawBlendMode(SDL_Renderer* renderer, SDL_BlendMode* blend_mode);
int SDL_RenderClear(SDL_Renderer* renderer);
int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y);
void SDL_RenderPresent(SDL_Renderer* renderer);
int SDL_RenderSetIntegerScale(SDL_Renderer* renderer, int enabled);
void SDL_RenderSetViewport(SDL_Renderer* renderer, const SDL_Rect* rect);

int SDL_PollEvent(SDL_Event* event);
void SDL_PumpEvents(void);
const Uint8* SDL_GetKeyboardState(int* numkeys);
const Uint8* SDL_GetKeyState(int* numkeys);
Uint16 SDL_GetModState(void);
void SDL_SetModState(Uint16 modstate);
int SDL_EnableKeyRepeat(int delay_ms, int interval_ms);
void SDL_QOS_SetKeyRepeat(int enabled, Uint32 delay_ms, Uint32 interval_ms);
Uint32 SDL_GetMouseState(int* x, int* y);
Uint32 SDL_GetRelativeMouseState(int* x, int* y);
Uint32 SDL_GetGlobalMouseState(int* x, int* y);
int SDL_QOS_GetMouseWheel(int* x, int* y);

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h);
SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface);
void SDL_DestroyTexture(SDL_Texture* texture);
int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access, int* w, int* h);
int SDL_SetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode blend_mode);
int SDL_GetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode* blend_mode);
int SDL_SetTextureAlphaMod(SDL_Texture* texture, Uint8 alpha);
int SDL_GetTextureAlphaMod(SDL_Texture* texture, Uint8* alpha);
int SDL_SetTextureColorMod(SDL_Texture* texture, Uint8 r, Uint8 g, Uint8 b);
int SDL_GetTextureColorMod(SDL_Texture* texture, Uint8* r, Uint8* g, Uint8* b);
int SDL_LockTexture(SDL_Texture* texture, const SDL_Rect* rect, void** pixels, int* pitch);
void SDL_UnlockTexture(SDL_Texture* texture);
int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch);
int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst);
int SDL_RenderCopyEx(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst, int angle_degrees, const SDL_Point* center, SDL_RendererFlip flip);
int SDL_RenderTexture(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst);
SDL_Surface* SDL_LoadBMP(const char* file);
SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format);
void SDL_FreeSurface(SDL_Surface* surface);
void SDL_DestroySurface(SDL_Surface* surface);
int SDL_FillRect(SDL_Surface* surface, const SDL_Rect* rect, Uint32 color);
int SDL_SetSurfaceColorKey(SDL_Surface* surface, int enabled, Uint32 key);
int SDL_SetColorKey(SDL_Surface* surface, int flag, Uint32 key);
int SDL_GetSurfaceColorKey(SDL_Surface* surface, Uint32* key);
Uint32 SDL_MapRGB(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b);
Uint32 SDL_MapRGBA(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b, Uint8 a);

#ifdef QOS_USERSPACE
static inline Uint64 SDL_GetTicksNS(void){
    return (Uint64)qos_get_time_ns();
}

static inline Uint64 SDL_GetPerformanceCounter(void){
    return (Uint64)qos_get_counter_cycles();
}

static inline Uint64 SDL_GetPerformanceFrequency(void){
    return (Uint64)qos_get_counter_hz();
}
#endif

#ifdef __cplusplus
}
#endif

#endif
