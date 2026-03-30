ENTRY(_start)

SECTIONS
{
  . = 0x04000000;

  .text : ALIGN(64) {
    KEEP(*(.text.entry))
    . = ALIGN(2048);
    KEEP(*(.text.exceptions))
    *(.text .text.*)
  }

  .rodata : ALIGN(64) {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(64) {
    *(.data .data.*)
  }

  . = ALIGN(64);
  __image_end = .;

  .bss (NOLOAD) : ALIGN(64) {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(64);
    __stack_bottom = .;
    . = . + 0x4000;
    __stack_top = .;
    __bss_end = .;
  }
}
