OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64K
}

SECTIONS
{
  .text.init : { *(.text.init) } > RAM

  . = ALIGN(64);
  .tohost : { *(.tohost) } > RAM

  .text   : { *(.text*) } > RAM
  .rodata : { *(.rodata*) *(.srodata*) } > RAM

  .data : {
    __global_pointer$ = . + 0x800;
    *(.data*) *(.sdata*)
  } > RAM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start = .;
    *(.bss*) *(.sbss*) *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  . = ALIGN(16);
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);

  /DISCARD/ : { *(.eh_frame) *(.riscv.attributes) *(.comment) }
}