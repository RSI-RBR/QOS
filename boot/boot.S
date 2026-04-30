.section .text.boot

.global _start

_start:
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    cbz x0, master
hang:
    b hang

master:
    // Clear BSS
    ldr x0, =bss_begin
    ldr x1, =bss_end
    sub x1, x1, x0
    bl memzero

    // Set stack pointer
    ldr x0, =stack_top
    mov sp, x0

    // 🔴 Initialize stack guard at bottom
    ldr x0, =stack_bottom
    ldr x1, =0xAAAAAAAA
    str x1, [x0]

    // Ensure kernel runs in EL1h.
    mrs x0, CurrentEL
    cmp x0, #0x8
    b.ne enter_kernel

    // Drop EL2 -> EL1
    mov x0, #(1 << 31)      // HCR_EL2.RW = 1 (EL1 AArch64)
    msr hcr_el2, x0
    isb

    msr sctlr_el1, xzr
    ldr x0, =stack_top
    msr sp_el1, x0

    adr x0, enter_kernel
    msr elr_el2, x0

    mov x0, #0x3C5          // EL1h with DAIF masked
    msr spsr_el2, x0
    isb
    eret

enter_kernel:
    ldr x0, =stack_top
    mov sp, x0
    bl kernel_main

loop:
    b loop
