.global run_program

run_program:
    // Save callee-saved registers
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x29, sp

    // Save program pointer
    mov x19, x0

    // Pass argument
    mov x0, x2

    // Call program
    blr x19

    // Restore registers
    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    ret
