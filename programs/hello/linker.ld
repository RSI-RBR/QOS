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
    . = ALIGN(4096);
    __qos_data_start = .;
    .data : { *(.data*) } :data

    .bss : {
        *(.bss*)
        *(COMMON)
    } :data
    __qos_image_end = .;
}
