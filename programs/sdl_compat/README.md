QOS SDL Compatibility Shim
==========================

This folder contains a freestanding SDL-style shim for QOS user programs.

Files:
- `sdl_shim.c`: implementation of a minimal SDL renderer/event/timer subset.
- Uses `include/SDL.h` and QOS syscalls (`QOS_USERSPACE`).

Current supported core APIs:
- Init/Quit: `SDL_Init`, `SDL_InitSubSystem`, `SDL_Quit`, `SDL_GetError`, `SDL_GetBasePath`, `SDL_free`, `SDL_SetHint`
- Timing: `SDL_GetTicks`, `SDL_GetTicks64`, `SDL_Delay`, `SDL_GetTicksNS`, `SDL_GetPerformanceCounter`, `SDL_GetPerformanceFrequency`, `SDL_TICKS_PASSED`
- Window/Renderer: `SDL_CreateWindow`, `SDL_DestroyWindow`, `SDL_GetCurrentDisplayMode`, `SDL_CreateRenderer`, `SDL_DestroyRenderer`, `SDL_RenderSetIntegerScale`, `SDL_RenderSetViewport`
- Draw: `SDL_SetRenderDrawColor`, `SDL_RenderClear`, `SDL_RenderFillRect`, `SDL_RenderDrawRect`, `SDL_RenderDrawPoint`, `SDL_RenderPresent`
- Input: `SDL_PollEvent`, `SDL_TEXTINPUT`, `SDL_PumpEvents`, `SDL_GetKeyboardState`, `SDL_GetKeyState`, `SDL_GetModState`, `SDL_GetMouseState`, `SDL_GetRelativeMouseState`, `SDL_GetGlobalMouseState`, `SDL_QOS_GetMouseWheel`
- Texture: `SDL_CreateTexture`, `SDL_DestroyTexture`, `SDL_QueryTexture`, `SDL_LockTexture`, `SDL_UnlockTexture`, `SDL_UpdateTexture`, `SDL_RenderCopy`, `SDL_RenderCopyEx`, `SDL_RenderTexture`
- Blending: `SDL_SetTextureBlendMode`, `SDL_SetTextureAlphaMod`, `SDL_SetTextureColorMod`, plus renderer blend-mode API
- Surfaces/BMP assets: `SDL_LoadBMP`, `SDL_CreateRGBSurfaceWithFormat`, `SDL_CreateTextureFromSurface`, `SDL_FillRect`, `SDL_FreeSurface`, `SDL_DestroySurface`, `SDL_SetSurfaceColorKey`, `SDL_SetColorKey`, `SDL_MapRGB`, `SDL_MapRGBA`
- SDL_image-style BMP helpers: `IMG_Init`, `IMG_Quit`, `IMG_Load`, `IMG_LoadTexture`, `IMG_GetError`

Notes:
- Texture and surface memory uses the userspace heap (`malloc`/`free`).
- Set the program reservation large enough for sprite sheets and maps; the QOS sample app defaults to 16 MiB.
- Pixel format currently supports `SDL_PIXELFORMAT_RGBA8888`.
- `SDL_Surface::format` is pointer-compatible with SDL-style code, but it only carries the QOS RGBA8888 format id and 4-byte pixel size today.
- `SDL_RenderSetIntegerScale` and `SDL_RenderSetViewport` are compatibility no-ops while QOS uses a fullscreen framebuffer session.
- BMP loading supports uncompressed 24-bit and 32-bit BMP files, including 256x256 sprites.
- 32-bit BMP alpha is preserved. 24-bit BMP sprites can use color-key transparency, for example magenta via `SDL_SetColorKey(surface, SDL_TRUE, SDL_MapRGB(surface->format, 255, 0, 255))`.
- Asset paths are sandbox-relative. `GAME.BIN` is currently mapped to the `QF2D` folder, so `img/tile_grass.bmp` is read inside `QF2D/IMG/`.
- The FAT driver resolves short 8.3 names and ASCII FAT long filenames for sandboxed BMP assets.
- `SDL_RenderCopy` uses software nearest-neighbor scaling and alpha blending.
- `SDL_RenderCopyEx` currently supports horizontal/vertical flips; rotation is accepted but ignored for now.
- Keyboard state is scancode-based (`SDL_SCANCODE_*`) and supports held-key polling for games.
- Key repeat is available but disabled by default; call `SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL)` or `SDL_QOS_SetKeyRepeat(1, delay_ms, interval_ms)` if text-entry style repeat is desired.
- `SDL_PollEvent` clears the output event to type `0` when no event is available, which is slightly more forgiving than desktop SDL for early QOS ports.
- Mouse motion events include button state for drag handling. `SDL_GetRelativeMouseState` returns accumulated movement since the last call, and `SDL_QOS_GetMouseWheel` returns accumulated wheel movement since the last call.
- `gpu2d v3d on` enables the experimental true V3D command-list batch path for 64px-aligned solid fill quads. It does not accelerate arbitrary 32px/textured sprites yet.

QOS Static Terrain Chunk Cache:
- `SDL_QOS_CreateWorldChunkLayer(...)` creates an opt-in cache for static tile terrain.
- The game provides a `SDL_QOS_WorldTileCallback` that maps `(tile_x, tile_y)` to a terrain `SDL_Texture*` and optional source rect.
- `SDL_QOS_RenderWorldChunkLayer(layer, camera_x_px, camera_y_px, viewport_w, viewport_h, tile_px)` rebuilds only dirty/missing terrain chunks, then draws visible chunks to the renderer.
- `SDL_QOS_InvalidateWorldTile(...)`, `SDL_QOS_InvalidateWorldChunk(...)`, and `SDL_QOS_InvalidateWorldChunkLayer(...)` mark cached terrain dirty after map edits.
- Chunk pixel size is capped internally, so large zoom levels automatically use fewer tiles per chunk instead of allocating huge textures.
- Cached chunks are normal SDL textures and consume program heap/texture slots. Start with `max_cached_chunks` around 12 for a 1080p viewport, then tune after checking FPS and heap pressure.
- This is meant for opaque/static terrain. Dynamic units, selection highlights, fog, UI, and mouse cursors should still draw normally after the terrain layer.
