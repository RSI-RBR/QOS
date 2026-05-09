QOS Game Wrapper
================

This folder is the public QOS-side build wrapper for QuantumFront2D.
Private game source and assets should stay local and ignored by git.

How it works:
- QOS enters user programs through `program_main(void)`.
- Desktop SDL code usually enters through `main(int argc, char** argv)`.
- The Makefile compiles private game sources with `-Dmain=qf2d_main`.
- `program_main.c` calls `qf2d_main(1, argv)` and then exits through QOS.
- The Makefile force-includes `qos_stdio.h`, mapping `printf`, `fprintf`,
  and `snprintf` to QOS-safe userspace logging/formatting functions.

Expected local layout:
- `programs/game/src/*.c`
- `programs/game/src/*.h`
- `programs/game/img/*.bmp` or the matching sandbox asset folder copied to SD.

Example:
```sh
mkdir -p programs/game/src programs/game/img
cp /path/to/QuantumFront2D/src/* programs/game/src/
cp /path/to/QuantumFront2D/img/* programs/game/img/
make -C programs/game SIGN_KEY=../../keys/dev_ed25519.pem PQ_SIGN_KEY=../../keys/dev_mldsa.key
```

Notes:
- `programs/game/src`, `programs/game/img`, and build outputs are ignored.
- The wrapper expects exactly one desktop entrypoint after preprocessing:
  `qf2d_main(int argc, char** argv)`.
- If the private tree has multiple source folders, override `GAME_SOURCES`.
- The game build allows floating-point code because QuantumFront2D already uses
  `double`. If we later run multiple FP-heavy apps at once, the scheduler should
  grow full FP/SIMD context save/restore.
