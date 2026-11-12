/* Section layout shared by pizero.ld (images at 0x8000) and pizero_loader.ld (the USB loader). */

SECTIONS
{
    .text : {
        KEEP(*(.text.boot))
        KEEP(*(.text.vectors))
        *(.text .text.*)
        *(.time_critical*)      /* Pico SDK RAM-placement sections (z4 pico_shim) */
    } > RAM

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    } > RAM

    .ARM.exidx : {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > RAM

    .data : ALIGN(8) {
        *(.data .data.*)
    } > RAM

    /* Known filler at the end of kernel.img. main checks it to find where the loaded image stops
     * matching the file. See PiZero_Z0_bringup.md section 6. */
    .loadcheck : ALIGN(16) {
        __loadcheck_start = .;
        FILL(0xA5A5A5A5);
        . += 8188;
        LONG(0x5A5A5A5A);
        __loadcheck_end = .;
    } > RAM

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.bss .bss.*)
        *(.slot_rings)          /* DSPi's output slot rings (z4), read only by the CPU */
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    /* DMA control blocks. mmu_enable() maps these whole megabytes as non-cacheable, so the DMA always
     * sees what the CPU wrote with no cache maintenance. */
    .nocache (NOLOAD) : ALIGN(0x100000) {
        __nocache_start = .;
        *(.nocache .nocache.*)
        . = ALIGN(0x100000);
        __nocache_end = .;
    } > RAM

    .stacks (NOLOAD) : ALIGN(16) {
        . += 4K;   __fiq_stack_top = .;
        . += 8K;   __irq_stack_top = .;
        . += 4K;   __abt_stack_top = .;
        . += 4K;   __und_stack_top = .;
        . += 256K; __sys_stack_top = .;
    } > RAM

    /* newlib's sbrk grows the heap upward from here. */
    . = ALIGN(16);
    end = .;
    _end = .;

    /DISCARD/ : { *(.comment) *(.note*) }
}
