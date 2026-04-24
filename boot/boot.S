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

    bl kernel_main

loop:
    b loop
