/*
 * AArch64 exception vector table and register-save glue for SpiLoader.
 *
 * Catches all exceptions, saves full register state into an
 * EXCEPTION_FRAME on the stack, and calls SpiloaderExceptionHandler()
 * to print diagnostics via UART.
 */

  .section .text.exceptions, "ax"

/*
 * Vector table -- must be 2 KB aligned (VBAR_ELx requirement).
 * 16 entries x 128 bytes each = 2048 bytes.
 */
  .align 11
  .global ExceptionVectorTable
ExceptionVectorTable:

/*
 * Each entry: allocate a 288-byte frame, save x0/x1, load the
 * vector index into x0, and branch to the common handler.
 * 4 instructions = 16 bytes, well within the 128-byte slot.
 */
.macro VECTOR_ENTRY id
  .align 7
  sub   sp, sp, #288
  stp   x0, x1, [sp]
  mov   x0, #\id
  b     ExceptionCommon
.endm

/* Current EL with SP_EL0 */
  VECTOR_ENTRY 0
  VECTOR_ENTRY 1
  VECTOR_ENTRY 2
  VECTOR_ENTRY 3

/* Current EL with SP_ELx  (the ones we actually use) */
  VECTOR_ENTRY 4
  VECTOR_ENTRY 5
  VECTOR_ENTRY 6
  VECTOR_ENTRY 7

/* Lower EL using AArch64 */
  VECTOR_ENTRY 8
  VECTOR_ENTRY 9
  VECTOR_ENTRY 10
  VECTOR_ENTRY 11

/* Lower EL using AArch32 */
  VECTOR_ENTRY 12
  VECTOR_ENTRY 13
  VECTOR_ENTRY 14
  VECTOR_ENTRY 15

/*
 * Common handler.
 *
 * On entry:
 *   x0  = vector index (0..15)
 *   [sp+0..15] = original {x0, x1}
 *   SP already decremented by 288
 *
 * Frame layout (288 bytes, 16-byte aligned):
 *   [sp+  0] x0   .. [sp+240] x30
 *   [sp+248] SP (pre-exception)
 *   [sp+256] ELR_ELx
 *   [sp+264] ESR_ELx
 *   [sp+272] FAR_ELx
 *   [sp+280] SPSR_ELx
 */
ExceptionCommon:
  /* Save x2..x30 (x0/x1 already saved by vector entry) */
  stp   x2,  x3,  [sp, #16]
  stp   x4,  x5,  [sp, #32]
  stp   x6,  x7,  [sp, #48]
  stp   x8,  x9,  [sp, #64]
  stp   x10, x11, [sp, #80]
  stp   x12, x13, [sp, #96]
  stp   x14, x15, [sp, #112]
  stp   x16, x17, [sp, #128]
  stp   x18, x19, [sp, #144]
  stp   x20, x21, [sp, #160]
  stp   x22, x23, [sp, #176]
  stp   x24, x25, [sp, #192]
  stp   x26, x27, [sp, #208]
  stp   x28, x29, [sp, #224]
  str   x30,      [sp, #240]

  /* Original SP = current SP + 288 (before the frame allocation) */
  add   x1, sp, #288
  str   x1, [sp, #248]

  /* Read system registers -- must match current exception level */
  mrs   x2, CurrentEL
  ubfx  x2, x2, #2, #2
  cmp   x2, #3
  b.eq  .Lel3_regs
  cmp   x2, #2
  b.eq  .Lel2_regs

  /* EL1 */
  mrs   x1, elr_el1
  str   x1, [sp, #256]
  mrs   x1, esr_el1
  str   x1, [sp, #264]
  mrs   x1, far_el1
  str   x1, [sp, #272]
  mrs   x1, spsr_el1
  str   x1, [sp, #280]
  b     .Lcall

.Lel2_regs:
  mrs   x1, elr_el2
  str   x1, [sp, #256]
  mrs   x1, esr_el2
  str   x1, [sp, #264]
  mrs   x1, far_el2
  str   x1, [sp, #272]
  mrs   x1, spsr_el2
  str   x1, [sp, #280]
  b     .Lcall

.Lel3_regs:
  mrs   x1, elr_el3
  str   x1, [sp, #256]
  mrs   x1, esr_el3
  str   x1, [sp, #264]
  mrs   x1, far_el3
  str   x1, [sp, #272]
  mrs   x1, spsr_el3
  str   x1, [sp, #280]

.Lcall:
  /* x0 = vector index, x1 = &frame */
  mov   x1, sp
  bl    SpiloaderExceptionHandler

  /* Should not return, but just in case */
.Lhang:
  wfi
  b     .Lhang
