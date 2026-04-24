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
        *(.text.boot)
    } :text

    .text : {
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
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

    /* Stack */
    stack_bottom = .;
    . += 0x10000;
    stack_top = .;
}
