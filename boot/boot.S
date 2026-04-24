.section .text.boot

.global _start

_start:
    // Only run on core 0
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    cbz x0, master

hang:
    b hang

master:
    // Zero BSS
    ldr x0, =bss_begin
    ldr x1, =bss_end
    sub x1, x1, x0
    bl memzero

    // Set stack pointer
    ldr x0, =stack_top
    mov sp, x0

    // ----------------------------------
    // Fill stack with guard pattern (0xAA)
    // ----------------------------------
    ldr x0, =stack_bottom
    ldr x1, =stack_top
    mov w2, #0xAA

fill_stack:
    cmp x0, x1
    b.ge fill_done
    strb w2, [x0], #1
    b fill_stack

fill_done:
    // ----------------------------------

    bl kernel_main

loop:
    b loop
