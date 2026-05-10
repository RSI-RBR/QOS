QOS User Program Library
========================

This folder contains reusable freestanding code for user programs.

Userspace heap:
- Link `../lib/qos_user_heap.c` into each program.
- Include `<stdlib.h>` for `malloc`, `free`, `calloc`, and `realloc`.
- Set `PROGRAM_MEMORY_BYTES` in the program Makefile to choose the signed
  process reservation. It must be a numeric byte value, aligned to 2 MiB,
  up to 32 MiB. Example: `PROGRAM_MEMORY_BYTES ?= 16777216`.
- Optional debug helpers are declared in `qos_user_heap.h`:
  - `qos_heap_total()`
  - `qos_heap_used()`
  - `qos_heap_free()`
  - `qos_heap_largest_free()`

The heap starts after the runtime-adjusted `__qos_image_end` and stops before
the process stack guard page inside that reservation. It is private to the
process address space and is wiped when the kernel destroys the program memory
reservation.
