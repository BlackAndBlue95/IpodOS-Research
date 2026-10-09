/* Region E: one contiguous block at E_BASE. The OS startup only copies, so bss is included
   as zeros and comes out initialised. */
SECTIONS
{
    .e 0x08b33000 : SUBALIGN(4) {
        __e_start = .;
        KEEP(*(.text.entry))
        KEEP(*(.rodata.keep))
        *(.text*) *(.rodata*) *(.data*)
        . = ALIGN(4);
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __e_end = .;
    }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
