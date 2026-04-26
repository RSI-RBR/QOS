.global _start
.extern program_main

_start:
    stp x29, x30, [sp, -16]!
    mov x29, sp
    // x0 already contains kernel_api_t*
    bl program_main

    ldp x29, x30, [sp], 16

    ret
