/* ring3_native.ld - link script for the freestanding ring-3 probe blobs the
 * kernel embeds (ring3_native_probe, ring3_thread_lifecycle_probe).
 *
 * The linked ELF is only an intermediate: objcopy flattens it to a raw binary
 * that the kernel loads at whatever user virtual address it picks. That is why
 * the origin is 0 and why code and rodata share one section - the blob has to be
 * position independent and a single contiguous run of bytes with no gaps to
 * relocate, and _start has to end up as its first byte, which is why each probe
 * puts it in .text.start and that section comes first. Zero-initialised statics
 * (.bss) start on the page after the code: the loader maps the code read+exec
 * and the rest of the linear region zeroed and read+write
 * (kernel_ring3_probe_runtime.c), and .bss is NOLOAD, so the flat image stays
 * code only. Initialised writable data (.data) has no place in the image. */
ENTRY(_start)

SECTIONS
{
  . = 0;

  .text :
  {
    *(.text.start)
    *(.text*)
    *(.rodata*)
  }

  . = ALIGN(4096);
  .bss (NOLOAD) :
  {
    *(.bss*)
    *(COMMON)
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
