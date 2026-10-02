/* Flat image linked at the usual DOL text address. GPL-2.0-or-later */
ENTRY(_start)
SECTIONS
{
  . = 0x80003100;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.sdata2*) }
  .data : { *(.data*) *(.sdata*) *(.sbss*) *(.bss*) *(COMMON) . = ALIGN(32); }
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
