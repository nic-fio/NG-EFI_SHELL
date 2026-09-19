/* Linker script for NESH: ELF image at base 0, page-aligned sections.
 * tools/elf2efi.py turns the result into a PE32+ EFI application. */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)
SECTIONS
{
  . = SIZEOF_HEADERS;
  .hash     : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  .dynsym   : { *(.dynsym) }
  .dynstr   : { *(.dynstr) }
  .rela     : { *(.rela.*) }
  .dynamic  : { *(.dynamic) }

  . = ALIGN(4096);
  .text   : { *(.text .text.*) }
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(4096);
  .data   : { *(.data .data.*) *(.got .got.plt) }
  . = ALIGN(4096);
  .bss    : { *(.bss .bss.*) *(COMMON) }

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) }
}
