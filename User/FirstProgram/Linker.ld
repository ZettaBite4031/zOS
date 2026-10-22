OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R | X */
    data PT_LOAD FLAGS(6); /* R | W */
}

SECTIONS
{
    . = 0x0000000040000000;

    .text : ALIGN(0x1000)
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(0x1000);

    .data : ALIGN(0x1000)
    {
        KEEP(*(.zos_state))
        *(.data .data.*)
    } :data

    .bss (NOLOAD) :
    {
        *(.bss .bss.* COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
