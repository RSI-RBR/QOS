.global run_program

// x0 = program entry
// x1 = stack
// x2 = api

run_program:
    // Save kernel state
    mov x3, sp
    mov x4, x30

    // Switch stack
    mov sp, x1

    // Save program entry
    mov x5, x0

    // Set argument
    mov x0, x2

    // Call program
    blr x5

    // Restore state
    mov sp, x3
    mov x30, x4

    ret
