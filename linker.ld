ENTRY(start_stage2)
#OUTPUT_FORMAT(binary)

SECTIONS
{
    . = 0x9000;

    .text : ALIGN(4)
    {
        *(.text)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata)
    }

    .bss : ALIGN(4)
    {
        *(COMMON)
        *(.bss)
    }
}