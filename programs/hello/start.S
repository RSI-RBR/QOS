.global _start

_start:
    // x0 already contains kernel_api_t*
    bl program_main

1:
    b 1
