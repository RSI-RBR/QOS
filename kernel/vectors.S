.global vectors
.global irq_handler
.global restore_context_and_eret

.align 11
vectors:
    // Current EL with SP_EL0
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .

    // Current EL with SP_ELx
    .align 7
    b .
    .align 7
    b irq_entry
    .align 7
    b .
    .align 7
    b .

    // Lower EL using AArch64
    .align 7
    b .
    .align 7
    b irq_entry
    .align 7
    b .
    .align 7
    b .

    // Lower EL using AArch32
    .align 7
    b .
    .align 7
    b irq_entry
    .align 7
    b .
    .align 7
    b .

irq_entry:
    // Frame layout (272 bytes):
    // x0-x29 at 0..239, x30 at 240, ELR_EL1 at 248, SPSR_EL1 at 256, pad at 264
    sub sp, sp, #272

    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30, [sp, #240]

    mrs x1, elr_el1
    str x1, [sp, #248]
    mrs x1, spsr_el1
    str x1, [sp, #256]

    mov x0, sp
    bl irq_handler
    b restore_from_frame

restore_context_and_eret:
    // x0 = prebuilt frame pointer
restore_from_frame:
    mov sp, x0

    ldr x1, [sp, #248]
    msr elr_el1, x1
    ldr x1, [sp, #256]
    msr spsr_el1, x1

    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ldp x4, x5, [sp, #16 * 2]
    ldp x6, x7, [sp, #16 * 3]
    ldp x8, x9, [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30, [sp, #240]

    add sp, sp, #272
    eret
