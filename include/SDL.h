#ifndef QOS_SDL_H
#define QOS_SDL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char Uint8;
typedef unsigned short Uint16;
typedef unsigned int Uint32;

typedef struct SDL_Window {
    int w;
    int h;
    Uint32 flags;
    int alive;
} SDL_Window;

typedef struct SDL_Renderer {
    SDL_Window* window;
    Uint32 draw_color;
    int alive;
} SDL_Renderer;

typedef struct SDL_Texture {
    int w;
    int h;
    Uint32 format;
    int alive;
} SDL_Texture;

typedef struct SDL_Rect {
    int x;
    int y;
    int w;
    int h;
} SDL_Rect;

typedef struct SDL_Keysym {
    int sym;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_QuitEvent {
    Uint32 type;
} SDL_QuitEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_QuitEvent quit;
} SDL_Event;

#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_VIDEO 0x00000020u

#define SDL_WINDOW_FULLSCREEN 0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u

#define SDL_RENDERER_SOFTWARE 0x00000001u
#define SDL_RENDERER_ACCELERATED 0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u

#define SDL_QUIT 0x100u
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u

#define SDLK_ESCAPE 27

int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_Quit(void);

const char* SDL_GetError(void);

Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window* window);

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer* renderer);

int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_RenderClear(SDL_Renderer* renderer);
int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y);
void SDL_RenderPresent(SDL_Renderer* renderer);

int SDL_PollEvent(SDL_Event* event);
const Uint8* SDL_GetKeyboardState(int* numkeys);

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture* texture);
int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst);

#ifdef __cplusplus
}
#endif

#endif
