OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(KernelMain)

PHDRS 
{
    text    PT_LOAD FLAGS(5);
    rodata  PT_LOAD FLAGS(4);
    data    PT_LOAD FLAGS(6);
}

SECTIONS 
{
    /*
     * Temporary bring-up address. The first ELF loader can identity-map and
     * enter this image without defining the permanent higher-half layout.
    */
    . = 0x00100000;
    __KernelImageStart = .;

    .text ALIGN(0x1000) :
    {
        KEEP(*(.text.KernelMain))
        *(.text .text.*)
    } :text

    .rodata ALIGN(0x1000) :
    {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(0x1000) :
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(0x1000) (NOLOAD) :
    {
        __BssStart = .;
        *(.bss .bss.*)
        *(COMMON)
        __BssEnd = .;
    } : data

    __KernelImageEnd = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.gcc_except_table .gcc_except_table.*)
        *(.note .note.*)
    }
}