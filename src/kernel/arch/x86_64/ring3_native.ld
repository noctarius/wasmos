/* ring3_native.ld - link script for the freestanding ring-3 probe blobs the
 * kernel embeds (ring3_native_probe, ring3_thread_lifecycle_probe).
 *
 * The linked ELF is only an intermediate: objcopy flattens it to a raw binary
 * that the kernel loads at whatever user virtual address it picks. That is why
 * the origin is 0 and why code and rodata share one section - the blob has to be
 * position independent and a single contiguous run of bytes with no gaps to
 * relocate, and _start has to end up as its first byte. No output section is
 * declared for .data or .bss, so a probe that wants writable static storage gets
 * whatever orphan placement the linker chooses rather than a guarantee. */
ENTRY(_start)

SECTIONS
{
  . = 0;

  .text :
  {
    *(.text*)
    *(.rodata*)
  }

  /* Nothing here survives into the flattened image, and any of it that did would
   * shift the code away from offset 0 or pad the blob with unmapped bytes. */

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.llvm*)
  }
}
