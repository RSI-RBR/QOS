.global process_start
.global context_switch

// x0 = entry
// x1 = stack
// x2 = api

process_start:
    // Save kernel context before switching stacks
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x29, sp

    // Save kernel stack
    mov x19, sp

    // Switch to process stack
    mov sp, x1

    // Create valid process frame
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x29, sp

    // Preserve entry + api
    mov x20, x0
    mov x21, x2

    // Pass api as arg0
    mov x0, x21

    // Run user program
    blr x20

    // Program returned naturally

    // Tear down process frame
    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    // Restore kernel stack
    mov sp, x19

    // Restore kernel context
    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    ret


// x0 = &old->sp
// x1 = new->sp
context_switch:
    // Save outgoing process context
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x2, sp
    str x2, [x0]

    // Load incoming process stack
    mov sp, x1

    // Restore incoming process context
    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    ret
