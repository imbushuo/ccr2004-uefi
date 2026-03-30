  .section .text.entry, "ax"
  .align  7
  .global _start
  .type   _start, %function

_start:
  /* Clear BSS */
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
  /* Set up stack */
  adrp  x0, __stack_top
  add   x0, x0, :lo12:__stack_top
  mov   sp, x0
  mov   x29, xzr

  /* Install exception vector table (detect EL) */
  adrp  x0, ExceptionVectorTable
  add   x0, x0, :lo12:ExceptionVectorTable
  mrs   x1, CurrentEL
  ubfx  x1, x1, #2, #2
  cmp   x1, #3
  b.eq  .Lvbar_el3
  cmp   x1, #2
  b.eq  .Lvbar_el2
  msr   vbar_el1, x0
  b     .Lvbar_done
.Lvbar_el2:
  msr   vbar_el2, x0
  b     .Lvbar_done
.Lvbar_el3:
  msr   vbar_el3, x0
.Lvbar_done:
  isb

  bl    SpiloaderMain
3:
  wfi
  b     3b

  .size _start, . - _start
