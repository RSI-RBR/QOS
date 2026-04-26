.global _start
.extern program_main

_start:
    mov x29, sp
    // x0 already contains kernel_api_t*
    bl program_main

    ret
