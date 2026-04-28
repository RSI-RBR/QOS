.global context_switch

// x0 = old process struct pointer
// x1 = new process struct pointer

context_switch:

    // Save old callee-saved registers
    stp x19, x20, [x0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]

    // Save SP
    mov x2, sp
    str x2, [x0, #96]

    // Load new SP
    ldr x2, [x1, #96]
    mov sp, x2

    // Restore new registers
    ldp x19, x20, [x1]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]

    ret
