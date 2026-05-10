QOS Game Wrapper
================

This folder is the public QOS-side build wrapper for QuantumFront2D.
Private game source and assets should stay local and ignored by git.
If the private sources are not present, this wrapper deliberately skips
`GAME.BIN` so public QOS builds can still complete.

How it works:
- QOS enters user programs through `program_main(void)`.
- Desktop SDL code usually enters through `main(int argc, char** argv)`.
- The Makefile compiles private game sources with `-Dmain=qf2d_main`.
- The QOS entrypoint is explicit: `GAME_ENTRY=gui_main.c` by default.
  `test_main.c` is excluded by default so an older SDL test entrypoint cannot
  accidentally become the QOS game.
- Game code is built with hidden symbols/no-PLT because QOS loads raw program
  images and does not apply ELF dynamic relocations.
- `program_main.c` calls `qf2d_main(0, 0)` and then exits through QOS.
- The Makefile force-includes `qos_stdio.h` for private game C sources,
  mapping `printf`, `fprintf`, and `snprintf` to QOS-safe userspace
  logging/formatting functions.
- Desktop `FILE*` reads are stubbed for now. Missing text files behave like
  EOF, while `/dev/urandom` returns lightweight pseudo-random bytes so early map
  generation code does not read uninitialized data.
- `qos_isqrt_u64()` is linked for deterministic fixed-point distance math.
- A small `sqrt(double)` compatibility wrapper is present so the current desktop
  code can link while hot paths are migrated away from double.

Expected local layout:
- Either `programs/game/src/*.c` and `programs/game/src/*.h` locally, or an
  external private checkout passed with `QF2D_ROOT=/path/to/QuantumFront2D`.
- `programs/game/img/*.bmp` locally, or `QF2D_ROOT/img` copied by the full SD
  build script.

Example:
```sh
mkdir -p programs/game/src programs/game/img
cp /path/to/QuantumFront2D/src/* programs/game/src/
cp /path/to/QuantumFront2D/img/* programs/game/img/
make -C programs/game SIGN_KEY=../../keys/dev_ed25519.pem PQ_SIGN_KEY=../../keys/dev_mldsa.key
```

Full SD build with the private source outside this public repo:
```sh
QF2D_ROOT=/path/to/QuantumFront2D SD_MOUNT=/media/sd bash tools/build_and_copy_sd.sh
```

If the game entrypoint is renamed later:
```sh
QF2D_ROOT=/path/to/QuantumFront2D GAME_ENTRY=new_gui_entry.c SD_MOUNT=/media/sd bash tools/build_and_copy_sd.sh
```

The script stages private source under ignored `build/private_game_src`, builds
`GAME.BIN`, copies `GAME.BIN`/`GAME.PQS`, and copies BMP assets into
`QF2D/IMG` on the SD card. If private source is absent or a fresh game binary
is not produced, it skips the game and removes stale `GAME.BIN`/`GAME.PQS`
from the SD card so the shell cannot accidentally launch an older build.

Notes:
- `programs/game/src`, `programs/game/img`, and build outputs are ignored.
- The default game reservation is 16 MiB, matching the current kernel
  `QOS_PROGRAM_MAX_MEMORY_BYTES` limit.
- QOS fault reports now print `PROG_BASE` and `ELR_OFF`. On the Linux build
  machine, resolve a game crash with:
  `python3 tools/qos_fault_addr2line.py programs/game/game.elf <ELR> --base <PROG_BASE>`.
- The wrapper expects exactly one desktop entrypoint after preprocessing:
  `qf2d_main(int argc, char** argv)`.
- If the private tree has multiple source folders, override `GAME_SOURCES`.
- The game build allows floating-point code because QuantumFront2D already uses
  `double`. If we later run multiple FP-heavy apps at once, the scheduler should
  grow full FP/SIMD context save/restore.
- Real game file I/O should use QOS sandboxed asset APIs later. The current
  `FILE*` layer is only a compile/runtime safety shim for developer-only init
  text files.
- For range checks, prefer squared-distance compares. Use `qos_isqrt_u64()` only
  when the actual distance value is needed, such as normalizing movement.
