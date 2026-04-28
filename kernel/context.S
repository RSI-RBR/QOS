.global process_start
.global context_switch

// x0 = entry
// x1 = stack pointer
// x2 = api

process_start:
    mov sp, x1
    mov x19, x2
    mov x20, x0

    mov x0, x19
    blr x20

    bl process_exit_current

1:
    b 1b


// x0 = old process sp storage location
// x1 = new process sp
context_switch:
    // Save old context
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x2, sp
    str x2, [x0]

    // Load new stack
    mov sp, x1

    // Restore new context
    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    ret
