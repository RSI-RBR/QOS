.global run_program

run_program:
    // x0 = program
    // x2 = api

    mov x3, x0     // save program
    mov x0, x2     // set argument

    blr x3         // call program

    ret
