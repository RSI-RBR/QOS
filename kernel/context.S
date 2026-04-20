.global run_program

// x0 = program entry
// x1 = new stack
// x2 = api pointer

run_program:
    // Save kernel state
    mov x3, sp
    mov x5, x30

    // Switch to program stack
    mov sp, x1

    // Prepare call
    mov x4, x0      // program entry
    mov x0, x2      // argument (api)

    blr x4          // call program

    // Restore kernel state
    mov sp, x3
    mov x30, x5

    ret
