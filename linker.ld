OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4096) {
        *(.text.hot*)
        *(.text*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata*)
    }

    .requests : {
        KEEP(*(.requests_start))
        KEEP(*(.requests))
        KEEP(*(.requests_end))
    }

    .data : ALIGN(4096) {
        *(.data*)
    }

    .bss : ALIGN(4096) {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}
