ENTRY(_start)

SECTIONS
{
  . = 0;

  .text :
  {
    *(.text*)
    *(.rodata*)
  }

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.llvm*)
  }
}
