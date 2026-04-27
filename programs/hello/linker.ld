ENTRY(_start)

PHDRS
{
    text PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0;

    .text : { *(.text*) } :text
    .rodata : {*(.rodata*) } :text
    .data : { *(.data*) } :data

    .bss : {
        *(.bss*)
        *(COMMON)
    } :data
}
