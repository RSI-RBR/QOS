.global process_start
.global context_switch

.extern process_exit_current

// x0 = entry
// x1 = stack
// x2 = api

process_start:
    // Switch to process stack
    mov sp, x1

    // Reserve initial frame
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x29, sp

    // Save args
    mov x19, x2      // api
    mov x20, x0      // entry

    // Pass api to program
    mov x0, x19

    // Run program
    blr x20

    // If program returns
    bl process_exit_current
    ret
1:
    b 1b


// x0 = &old->sp
// x1 = new->sp
context_switch:
    stp x19, x20, [sp, -16]!
    stp x21, x22, [sp, -16]!
    stp x23, x24, [sp, -16]!
    stp x25, x26, [sp, -16]!
    stp x27, x28, [sp, -16]!
    stp x29, x30, [sp, -16]!

    mov x2, sp
    str x2, [x0]

    mov sp, x1

    ldp x29, x30, [sp], 16
    ldp x27, x28, [sp], 16
    ldp x25, x26, [sp], 16
    ldp x23, x24, [sp], 16
    ldp x21, x22, [sp], 16
    ldp x19, x20, [sp], 16

    ret
