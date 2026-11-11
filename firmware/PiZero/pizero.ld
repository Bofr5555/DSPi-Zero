/* The GPU firmware copies kernel.img to 0x8000 as one flat block, so .data needs no copy at boot.
 * The ARM sees all of SDRAM below the GPU split. 256 MB is safe for any gpu_mem on a 512 MB Zero.
 * The USB loader (pizero_loader.ld) also places received images here. */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x8000, LENGTH = 256M - 0x8000
}

INCLUDE pizero_sections.ld
