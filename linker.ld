ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x80000;

    /* Code (read + execute) */
    .text.boot : {
        __kernel_text_start = .;
        *(.text.boot)
    } :text

    .text : {
        *(.text*)
        __kernel_text_end = .;
    } :text

    .rodata : {
        __kernel_rodata_start = .;
        *(.rodata*)
        __kernel_rodata_end = .;
    } :text

    /* Data (read + write) */
    .data : {
        *(.data*)
    } :data

    .bss : {
        . = ALIGN(16);
        bss_begin = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        bss_end = .;
    } :data

    . += 0x1000;
    . = ALIGN(16);

    /* Per-core stacks */
    core0_stack_bottom = .;
    . += 0x10000;
    core0_stack_top = .;

    core1_stack_bottom = .;
    . += 0x10000;
    core1_stack_top = .;

    core2_stack_bottom = .;
    . += 0x10000;
    core2_stack_top = .;

    core3_stack_bottom = .;
    . += 0x10000;
    core3_stack_top = .;

    /* Backward-compatible aliases used by existing code */
    stack_bottom = core0_stack_bottom;
    stack_top = core0_stack_top;
}
