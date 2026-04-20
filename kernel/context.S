.global run_program

run_program:
    // Save kernel state
    stp x29, x30, [sp, -16]!
    mov x29, sp

    mov x3, sp

    // Switch stack
    mov sp, x1

    // Create valid frame for program
    stp x29, x30, [sp, -16]!
    mov x29, sp

    // Save program entry
    mov x5, x0

    // Pass argument
    mov x0, x2

    blr x5

    // Tear down program frame
    ldp x29, x30, [sp], 16

    // Restore kernel stack
    mov sp, x3

    // Restore kernel frame
    ldp x29, x30, [sp], 16

    ret
