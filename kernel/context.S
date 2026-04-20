.global run_program

// x0 = program entry
// x1 = stack pointer
// x2 = api pointer

run_program:
    mov x3, sp // save current stack
    mov sp, x1 // switch to program stack

    mov x4, x0 // save program entry
    mov x0, x2 // api argument

    blr x4 // call program

    mov sp, x3 // restore stack
    ret
