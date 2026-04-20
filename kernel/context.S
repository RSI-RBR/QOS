.global run_program

// x0 = program entry
// x1 = stack
// x2 = api

run_program:
    // Save callee-saved registers
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x29, sp

    // Save kernel stack
    mov x19, sp

    // Switch to program stack
    mov sp, x1

    // Save program entry
    mov x20, x0

    // Pass argument
    mov x0, x2

    blr x20

    // Restore kernel stack
    mov sp, x19

    // Restore registers
    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    ret
