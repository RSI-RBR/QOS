QOS SDL Compatibility Shim
==========================

This folder contains a freestanding SDL-style shim for QOS user programs.

Files:
- `sdl_shim.c`: implementation of a minimal SDL renderer/event/timer subset.
- Uses `include/SDL.h` and QOS syscalls (`QOS_USERSPACE`).

Current supported core APIs:
- Init/Quit: `SDL_Init`, `SDL_InitSubSystem`, `SDL_Quit`, `SDL_GetError`
- Timing: `SDL_GetTicks`, `SDL_Delay`, `SDL_GetTicksNS`, `SDL_GetPerformanceCounter`, `SDL_GetPerformanceFrequency`
- Window/Renderer: `SDL_CreateWindow`, `SDL_DestroyWindow`, `SDL_CreateRenderer`, `SDL_DestroyRenderer`
- Draw: `SDL_SetRenderDrawColor`, `SDL_RenderClear`, `SDL_RenderFillRect`, `SDL_RenderDrawRect`, `SDL_RenderDrawPoint`, `SDL_RenderPresent`
- Input: `SDL_PollEvent`, `SDL_GetKeyboardState`, `SDL_GetMouseState`
- Texture: `SDL_CreateTexture`, `SDL_DestroyTexture`, `SDL_LockTexture`, `SDL_UnlockTexture`, `SDL_UpdateTexture`, `SDL_RenderCopy`

Notes:
- Texture memory uses a fixed internal pool in the shim.
- Pixel format currently supports `SDL_PIXELFORMAT_RGBA8888`.
- `SDL_RenderCopy` uses software nearest-neighbor scaling and alpha blending.
