.global vectors
.global irq_handler
.global restore_context_and_eret
.global sync_exception_handler

.align 11
vectors:
    // Current EL with SP_EL0
    .align 7
    b sync_entry
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .

    // Current EL with SP_ELx
    .align 7
    b sync_entry
    .align 7
    b irq_entry
    .align 7
    b .
    .align 7
    b .

    // Lower EL using AArch64
    .align 7
    b sync_entry
    .align 7
    b irq_entry
    .align 7
    b .
    .align 7
    b .

    // Lower EL using AArch32
    .align 7
    b sync_entry
    .align 7
    b irq_entry
    .align 7
    b .
    .align 7
    b .

irq_entry:
    // Frame layout (800 bytes):
    // x0-x29 at 0..239, x30 at 240, ELR_EL1 at 248, SPSR_EL1 at 256, SP_EL0 at 264
    // q0-q31 at 272..783, FPCR at 784, FPSR at 792
    sub sp, sp, #800

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
    mrs x1, sp_el0
    str x1, [sp, #264]

    stp q0, q1, [sp, #272]
    stp q2, q3, [sp, #304]
    stp q4, q5, [sp, #336]
    stp q6, q7, [sp, #368]
    stp q8, q9, [sp, #400]
    stp q10, q11, [sp, #432]
    stp q12, q13, [sp, #464]
    stp q14, q15, [sp, #496]
    stp q16, q17, [sp, #528]
    stp q18, q19, [sp, #560]
    stp q20, q21, [sp, #592]
    stp q22, q23, [sp, #624]
    stp q24, q25, [sp, #656]
    stp q26, q27, [sp, #688]
    stp q28, q29, [sp, #720]
    stp q30, q31, [sp, #752]
    mrs x1, fpcr
    str x1, [sp, #784]
    mrs x1, fpsr
    str x1, [sp, #792]

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
    ldr x1, [sp, #264]
    msr sp_el0, x1

    ldp q0, q1, [sp, #272]
    ldp q2, q3, [sp, #304]
    ldp q4, q5, [sp, #336]
    ldp q6, q7, [sp, #368]
    ldp q8, q9, [sp, #400]
    ldp q10, q11, [sp, #432]
    ldp q12, q13, [sp, #464]
    ldp q14, q15, [sp, #496]
    ldp q16, q17, [sp, #528]
    ldp q18, q19, [sp, #560]
    ldp q20, q21, [sp, #592]
    ldp q22, q23, [sp, #624]
    ldp q24, q25, [sp, #656]
    ldp q26, q27, [sp, #688]
    ldp q28, q29, [sp, #720]
    ldp q30, q31, [sp, #752]
    ldr x1, [sp, #784]
    msr fpcr, x1
    ldr x1, [sp, #792]
    msr fpsr, x1

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

    add sp, sp, #800
    eret

sync_entry:
    // Use same frame layout as IRQ path so sync handler can recover by scheduling.
    sub sp, sp, #800

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
    mrs x1, sp_el0
    str x1, [sp, #264]

    stp q0, q1, [sp, #272]
    stp q2, q3, [sp, #304]
    stp q4, q5, [sp, #336]
    stp q6, q7, [sp, #368]
    stp q8, q9, [sp, #400]
    stp q10, q11, [sp, #432]
    stp q12, q13, [sp, #464]
    stp q14, q15, [sp, #496]
    stp q16, q17, [sp, #528]
    stp q18, q19, [sp, #560]
    stp q20, q21, [sp, #592]
    stp q22, q23, [sp, #624]
    stp q24, q25, [sp, #656]
    stp q26, q27, [sp, #688]
    stp q28, q29, [sp, #720]
    stp q30, q31, [sp, #752]
    mrs x1, fpcr
    str x1, [sp, #784]
    mrs x1, fpsr
    str x1, [sp, #792]

    mov x0, sp
    mrs x1, esr_el1
    mrs x2, elr_el1
    mrs x3, spsr_el1
    bl sync_exception_handler
    b restore_from_frame
