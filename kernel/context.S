.global run_program

// x0 = program entry
// x1 = new stack
// x2 = api pointer

run_program:
    // Save full context we care about
    mov x3, sp
    mov x4, x30
    mov x5, x29

    // Switch stack
    mov sp, x1

    // Set up proper frame
    mov x29, sp

    // Prepare args
    mov x6, x0      // program entry
    mov x0, x2      // api

    blr x6          // call program

    // Restore context
    mov sp, x3
    mov x30, x4
    mov x29, x5

    ret
