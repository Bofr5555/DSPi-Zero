/* The USB loader runs at 32 MB (kernel_address in boot/config_loader.txt), so the image it receives
 * can go to 0x8000, where every milestone image is linked. Its BSS and stacks stay above 32 MB too. */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x2000000, LENGTH = 16M
}

INCLUDE pizero_sections.ld
