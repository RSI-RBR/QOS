ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text.boot : { *(.text.boot) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    .bss : {
        . = ALIGN(16);
        bss_begin = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        bss_end = .;
    }

    .stack (NOLOAD) : {
        . = ALIGN(16);
        stack_bottom = .;
        LONG(0xDEADBEEF);
        . += 0x100000;
        stack_top = .;

    }
}
