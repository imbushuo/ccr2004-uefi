  .section .text.entry, "ax"
  .align  7
  .global _start
  .type   _start, %function

_start:
  adrp  x0, __bss_start
  add   x0, x0, :lo12:__bss_start
  adrp  x1, __bss_end
  add   x1, x1, :lo12:__bss_end
  cmp   x0, x1
  b.hs  2f
1:
  str   xzr, [x0], #8
  cmp   x0, x1
  b.lo  1b
2:
  adrp  x0, __stack_top
  add   x0, x0, :lo12:__stack_top
  mov   sp, x0
  mov   x29, xzr
  bl    SpiloaderMain
3:
  wfi
  b     3b

  .size _start, . - _start
