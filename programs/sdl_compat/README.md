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
- Texture: `SDL_CreateTexture`, `SDL_DestroyTexture`, `SDL_QueryTexture`, `SDL_LockTexture`, `SDL_UnlockTexture`, `SDL_UpdateTexture`, `SDL_RenderCopy`, `SDL_RenderCopyEx`, `SDL_RenderTexture`
- Blending: `SDL_SetTextureBlendMode`, `SDL_SetTextureAlphaMod`, `SDL_SetTextureColorMod`, plus renderer blend-mode API
- BMP assets: `SDL_LoadBMP`, `SDL_CreateTextureFromSurface`, `SDL_FreeSurface`, `SDL_DestroySurface`, `SDL_SetSurfaceColorKey`, `SDL_SetColorKey`
- SDL_image-style BMP helpers: `IMG_Init`, `IMG_Quit`, `IMG_Load`, `IMG_LoadTexture`, `IMG_GetError`

Notes:
- Texture and surface memory uses the userspace heap (`malloc`/`free`).
- Set the program reservation large enough for sprite sheets and maps; the QOS sample app defaults to 16 MiB.
- Pixel format currently supports `SDL_PIXELFORMAT_RGBA8888`.
- BMP loading supports uncompressed 24-bit and 32-bit BMP files, including 256x256 sprites.
- 32-bit BMP alpha is preserved. 24-bit BMP sprites can use color-key transparency, for example magenta via `SDL_SetColorKey(surface, SDL_TRUE, SDL_MapRGB(surface->format, 255, 0, 255))`.
- Asset paths are sandbox-relative. `GAME.BIN` is currently mapped to the `QF2D` folder, so `img/tile_grass.bmp` is read inside `QF2D/IMG/`.
- The FAT driver resolves short 8.3 names and ASCII FAT long filenames for sandboxed BMP assets.
- `SDL_RenderCopy` uses software nearest-neighbor scaling and alpha blending.
- `SDL_RenderCopyEx` currently supports horizontal/vertical flips; rotation is accepted but ignored for now.
