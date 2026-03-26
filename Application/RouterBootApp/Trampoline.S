//
//  RouterBoot relocation trampoline.
//
//  Called from C after ExitBootServices:
//    RouterBootTrampoline(DtbAddress, SourceAddr, ImageSize)
//      x0 = DTB address
//      x1 = source address of RouterBoot binary
//      x2 = size in bytes
//
//  Sequence:
//    1. Mask interrupts (DAIF)
//    2. Copy RouterBoot to 0x00110000 (with MMU still on — both
//       source and target are identity-mapped in UEFI)
//    3. Clean+invalidate entire D-cache
//    4. Disable MMU, D-cache, I-cache
//    5. Invalidate I-cache and TLB
//    6. Jump to 0x00110000 with DTB in x0
//

#define ROUTERBOOT_LOAD_ADDR  0x00110000

.global RouterBootTrampoline
.type   RouterBootTrampoline, %function
RouterBootTrampoline:
  // Save arguments
  mov   x19, x0          // DTB address
  mov   x20, x1          // source
  mov   x21, x2          // size

  // 1. Mask all interrupts
  msr   daifset, #0xf

  // 2. Copy RouterBoot binary to target address (MMU still on)
  ldr   x3, =ROUTERBOOT_LOAD_ADDR
  mov   x4, x21          // remaining bytes
.Lcopy:
  cbz   x4, .Lcopy_done
  ldrb  w5, [x20], #1
  strb  w5, [x3], #1
  sub   x4, x4, #1
  b     .Lcopy
.Lcopy_done:

  // 3. Clean+invalidate entire D-cache (set/way)
  dmb   sy
  mrs   x0, clidr_el1
  ubfx  x3, x0, #24, #3
  cbz   x3, .Ldc_done
  mov   x8, xzr
.Ldc_level:
  add   x2, x8, x8, lsr #1
  lsr   x1, x0, x2
  and   x1, x1, #7
  cmp   x1, #2
  b.lt  .Ldc_skip
  msr   csselr_el1, x8
  isb
  mrs   x1, ccsidr_el1
  and   x2, x1, #7
  add   x2, x2, #4
  ubfx  x4, x1, #3, #10
  clz   w5, w4
  ubfx  x6, x1, #13, #15
.Ldc_set:
  mov   x7, x6
.Ldc_way:
  lsl   x9, x4, x5
  orr   x11, x8, x9
  lsl   x9, x7, x2
  orr   x11, x11, x9
  dc    cisw, x11
  subs  x7, x7, #1
  b.ge  .Ldc_way
  subs  x4, x4, #1
  b.ge  .Ldc_set
.Ldc_skip:
  add   x8, x8, #2
  cmp   x8, x3, lsl #1
  b.lt  .Ldc_level
.Ldc_done:
  dsb   sy

  // 4. Disable MMU, D-cache, I-cache
  mrs   x0, CurrentEL
  cmp   x0, #(2 << 2)
  b.eq  .Ldis_el2

  mrs   x0, sctlr_el1
  bic   x0, x0, #(1 << 0)    // M — MMU
  bic   x0, x0, #(1 << 2)    // C — D-cache
  bic   x0, x0, #(1 << 12)   // I — I-cache
  msr   sctlr_el1, x0
  isb
  b     .Ldis_done

.Ldis_el2:
  mrs   x0, sctlr_el2
  bic   x0, x0, #(1 << 0)
  bic   x0, x0, #(1 << 2)
  bic   x0, x0, #(1 << 12)
  msr   sctlr_el2, x0
  isb

.Ldis_done:
  // 5. Invalidate I-cache and TLB
  ic    iallu
  tlbi  vmalle1
  dsb   sy
  isb

  // 6. Jump to RouterBoot with DTB in x0
  mov   x0, x19
  mov   x1, xzr
  mov   x2, xzr
  mov   x3, xzr
  ldr   x4, =ROUTERBOOT_LOAD_ADDR
  br    x4
