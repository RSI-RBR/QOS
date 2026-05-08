.section .text.boot

.global _start
.global secondary_start

_start:
    mrs x19, mpidr_el1
    and x19, x19, #0xFF
    cbz x19, master
    b secondary_start

master:
    // Clear BSS once on core0.
    ldr x0, =bss_begin
    ldr x1, =bss_end
    sub x1, x1, x0
    bl memzero

    bl set_core_stack

    // Initialize stack guard at core0 stack bottom.
    ldr x0, =stack_bottom
    ldr x1, =0xAAAAAAAA
    str x1, [x0]

    // Ensure kernel runs in EL1h.
    mrs x0, CurrentEL
    cmp x0, #0x8
    b.ne enter_kernel

    // Enable cluster coherency in EL2 before enabling EL1 MMU/caches.
    // Matches the common armstub flow on Cortex-A53.
    mov x0, #(1 << 6)       // CPUECTLR_EL1.SMPEN
    msr s3_1_c15_c2_1, x0
    isb

    // Drop EL2 -> EL1
    mov x0, #(1 << 31)      // HCR_EL2.RW = 1 (EL1 AArch64)
    msr hcr_el2, x0
    isb

    // Allow EL1 access to physical counter/timer registers.
    mrs x0, cnthctl_el2
    orr x0, x0, #3          // EL1PCTEN=1, EL1PCEN=1
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    isb

    msr sctlr_el1, xzr
    mov x0, sp
    msr sp_el1, x0

    adr x0, enter_kernel
    msr elr_el2, x0

    mov x0, #0x3C5          // EL1h with DAIF masked
    msr spsr_el2, x0
    isb
    eret

enter_kernel:
    bl set_core_stack
    bl kernel_main

primary_hang:
    b primary_hang

secondary_start:
    // Secondary cores can be mailbox-jumped directly here; recompute core id.
    mrs x19, mpidr_el1
    and x19, x19, #0xFF
    bl set_core_stack

    // Ensure kernel runs in EL1h.
    mrs x0, CurrentEL
    cmp x0, #0x8
    b.ne secondary_enter_kernel

    // Enable cluster coherency in EL2 before enabling EL1 MMU/caches.
    mov x0, #(1 << 6)       // CPUECTLR_EL1.SMPEN
    msr s3_1_c15_c2_1, x0
    isb

    // Drop EL2 -> EL1
    mov x0, #(1 << 31)      // HCR_EL2.RW = 1 (EL1 AArch64)
    msr hcr_el2, x0
    isb

    // Allow EL1 access to physical counter/timer registers.
    mrs x0, cnthctl_el2
    orr x0, x0, #3          // EL1PCTEN=1, EL1PCEN=1
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    isb

    msr sctlr_el1, xzr
    mov x0, sp
    msr sp_el1, x0

    adr x0, secondary_enter_kernel
    msr elr_el2, x0

    mov x0, #0x3C5          // EL1h with DAIF masked
    msr spsr_el2, x0
    isb
    eret

secondary_enter_kernel:
    bl kernel_secondary_main

secondary_hang:
    b secondary_hang

set_core_stack:
    cmp x19, #0
    b.eq 2f
    cmp x19, #1
    b.eq 3f
    cmp x19, #2
    b.eq 4f
    cmp x19, #3
    b.eq 5f
    b secondary_hang
2:
    ldr x0, =core0_stack_top
    mov sp, x0
    ret
3:
    ldr x0, =core1_stack_top
    mov sp, x0
    ret
4:
    ldr x0, =core2_stack_top
    mov sp, x0
    ret
5:
    ldr x0, =core3_stack_top
    mov sp, x0
    ret
