#ifndef QOS_SDL_IMAGE_H
#define QOS_SDL_IMAGE_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_INIT_BMP 0x00000001u

int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Surface* IMG_Load(const char* file);
SDL_Texture* IMG_LoadTexture(SDL_Renderer* renderer, const char* file);
const char* IMG_GetError(void);

#ifdef __cplusplus
}
#endif

#endif
