#
#  Leaves Gunyah for EL2 once the OS loader's ExitBootServices succeeded, and
#  carries on at EL2 with UEFI's translation tables.
#
#  Gunyah takes SMC 0x02000121 (param 0x23, x4 = 1) as a request to hand EL2
#  over: it tears itself down and returns to the instruction after the smc,
#  at EL2, with the MMU off, HCR_EL2.E2H clear, VBAR_EL2 and SP_EL2 set to the
#  guest's VBAR_EL1 and SP_EL1, and x3-x30 as the guest had them; x0 is 0. It
#  cleans no data cache and masks no interrupt on the way. A refused call
#  returns at EL1: x0 4 for a second call in a boot, 0x37 when TrustZone's
#  feature query fails, 0x10 or 0x11 when the SMMU clean-up fails (the SMMU
#  is then in global bypass already); 0x11 or 0 also when Gunyah fails after
#  it has given EL2 up.
#
#  So, as the stock EnvDxe does: every dirty line goes to memory and the EL1
#  MMU and data cache are turned off before the call; afterwards, at EL2,
#  TTBR0, MAIR and TCR (converted to the EL2 format) are taken from EL1, the
#  caches cleaned and invalidated again, and the MMU and caches turned on.
#  UEFI's tables work in the EL2 regime: executable pages carry no XN bit,
#  and the UXN bit of the others is XN there.
#
#  Between the call and the MMU coming back on, data accesses are Device:
#  nothing may touch memory then, as stale lines may still be in the caches.
#  The breadcrumbs go straight to the GENI UART, which is MMIO.
#
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

#include <AsmMacroLib.h>
#include "../GunyahExit.h"

#define TZ_EL2_SWITCH_SMC_ID              0x02000121
#define TZ_EL2_SWITCH_PARAM_ID            0x23
#define TZ_EL2_SWITCH_PARAM2_EXIT_GUNYAH  1
#define QCOM_SCM_INTERRUPTED              1

#define UART_BASE           FixedPcdGet64 (PcdSerialRegisterBase)
#define GENI_STATUS         0x040
#define GENI_TX_TRANS_LEN   0x270
#define GENI_M_CMD0         0x600
#define GENI_TX_FIFO        0x700
#define GENI_M_CMD_TX       0x08000000

#define SCTLR_M   (1 << 0)
#define SCTLR_C   (1 << 2)

// SCTLR_EL2 without E2H: the RES1 bits, with I (instruction fetches stay
// cacheable while the MMU is off, as with what Gunyah leaves), and then M, C
// and SA on top.
#define SCTLR_EL2_RES1  0x30C50830
#define SCTLR_EL2_OFF   (SCTLR_EL2_RES1 | (1 << 12))
#define SCTLR_EL2_ON    (SCTLR_EL2_OFF | (1 << 0) | (1 << 2) | (1 << 3))

// HCR_EL2 without E2H or TGE, for a host at EL2: EL1 AArch64, physical
// interrupts to EL2, nothing trapped. Linux sets its own.
#define HCR_EL2_HOST  0x80000038

// CPTR_EL2 without E2H: RES1 bits, nothing trapped (UEFI's C code may use
// the FP/SIMD registers).
#define CPTR_EL2_NO_TRAPS  0x33FF

// TCR_EL1 fields TCR_EL2 has at the same place: T0SZ, IRGN0, ORGN0, SH0, TG0.
#define TCR_EL1_KEPT     0xFF3F
#define TCR_EL2_RES1     ((1 << 31) | (1 << 23))

// TTBR0 without ASID and CnP.
#define TTBR_BADDR_MASK  0x0000FFFFFFFFFFFE

#define CNTHV_CTL_EL2  S3_4_C14_C3_1

//
// Writes a character to the console, for the steps no C code can report.
// Clobbers x0-x2.
//
.macro BREADCRUMB Char
  MOV64 (x0, UART_BASE)
  mov   x2, #0x100000
1:ldr   w1, [x0, #GENI_STATUS]
  tbz   w1, #0, 2f
  subs  x2, x2, #1
  b.ne  1b
2:mov   w1, #1
  str   w1, [x0, #GENI_TX_TRANS_LEN]
  mov   w1, #GENI_M_CMD_TX
  str   w1, [x0, #GENI_M_CMD0]
  mov   w1, #\Char
  str   w1, [x0, #GENI_TX_FIFO]
.endm

//
// Cleans and invalidates every data and unified cache to the point of
// coherency by set and way. Only CPU 0 runs. Touches no memory and uses only
// x0-x8, x16 and x17.
//
GunyahExitTextStart:
GunyahExitCleanCaches:
  dsb   sy
  mrs   x0, clidr_el1
  ubfx  x3, x0, #24, #3           // x3 = LoC
  cbz   x3, 5f
  mov   x4, xzr                   // x4 = level
1:add   x5, x4, x4, lsl #1        // 3 bits of Ctype per level
  lsr   x5, x0, x5
  and   x5, x5, #7
  cmp   x5, #2                    // no data cache at this level
  b.lt  4f
  lsl   x6, x4, #1
  msr   csselr_el1, x6
  isb
  mrs   x6, ccsidr_el1
  and   x7, x6, #7
  add   x7, x7, #4                // x7 = log2 (line size)
  ubfx  x8, x6, #3, #10           // x8 = ways - 1
  ubfx  x16, x6, #13, #15         // x16 = sets - 1
  clz   w17, w8                   // x17 = way shift
2:mov   x2, x8                    // way
3:lsl   x1, x2, x17
  lsl   x6, x16, x7
  orr   x1, x1, x6
  orr   x1, x1, x4, lsl #1
  dc    cisw, x1
  subs  x2, x2, #1
  b.ge  3b
  subs  x16, x16, #1
  b.ge  2b
4:add   x4, x4, #1
  cmp   x4, x3
  b.lt  1b
5:msr   csselr_el1, xzr
  dsb   sy
  isb
  ret

//
// UINT64 GunyahExitSwitch (GUNYAH_EXIT_STATE *State)
//
ASM_FUNC(GunyahExitSwitch)
  stp   x29, x30, [sp, #-96]!
  mov   x29, sp
  stp   x19, x20, [sp, #16]
  stp   x21, x22, [sp, #32]
  stp   x23, x24, [sp, #48]
  stp   x25, x26, [sp, #64]
  stp   x27, x28, [sp, #80]

  mov   x19, x0
  msr   daifset, #0xf
  mov   x20, sp
  mrs   x21, ttbr0_el1
  mrs   x22, tcr_el1
  mrs   x23, mair_el1
  mrs   x24, vbar_el1
  mrs   x25, sctlr_el1
  str   x25, [x19, #GUNYAH_EXIT_EL1_SCTLR]
  str   x22, [x19, #GUNYAH_EXIT_EL1_TCR]
  str   x23, [x19, #GUNYAH_EXIT_EL1_MAIR]
  str   x21, [x19, #GUNYAH_EXIT_EL1_TTBR0]
  str   x24, [x19, #GUNYAH_EXIT_EL1_VBAR]
  str   x20, [x19, #GUNYAH_EXIT_EL1_SP]

  // This code to memory by address first, in case Gunyah ignores set/way
  // operations: it runs from memory while the caches are off.
  adr   x0, GunyahExitTextStart
  adr   x1, GunyahExitTextEnd
  mrs   x2, ctr_el0
  ubfx  x2, x2, #16, #4           // DminLine
  mov   x3, #4
  lsl   x2, x3, x2                // bytes per line
  sub   x3, x2, #1
  bic   x0, x0, x3
3:dc    civac, x0
  add   x0, x0, x2
  cmp   x0, x1
  b.lo  3b
  dsb   sy

  // Everything to memory, then the EL1 MMU and data cache off: Gunyah
  // cleans nothing and returns with the MMU off.
  bl    GunyahExitCleanCaches
  ic    iallu
  dsb   sy
  isb
  bic   x0, x25, #SCTLR_M
  bic   x0, x0, #SCTLR_C
  msr   sctlr_el1, x0
  isb

  BREADCRUMB 's'
  MOV64 (x0, TZ_EL2_SWITCH_SMC_ID)
  mov   x6, xzr
0:mov   x1, #TZ_EL2_SWITCH_PARAM_ID
  mov   x2, xzr
  mov   x3, xzr
  mov   x4, #TZ_EL2_SWITCH_PARAM2_EXIT_GUNYAH
  mov   x5, xzr
  smc   #0
  msr   daifset, #0xf             // Gunyah does not mask anything on the way
  mrs   x1, CurrentEL
  cmp   x1, #0x8
  b.eq  10f
  cmp   x0, #QCOM_SCM_INTERRUPTED
  b.eq  0b

  // Refused: still at EL1, nothing written meanwhile. Turn the MMU and data
  // cache back on as they were.
  mov   x27, x0
  BREADCRUMB '1'
  msr   sctlr_el1, x25
  isb
  mov   x0, #1
  str   x0, [x19, #GUNYAH_EXIT_CURRENT_EL]
  str   x27, [x19, #GUNYAH_EXIT_RESULT]
  b     20f

  // At EL2 with the MMU off: registers only until it is back on. Gunyah
  // ran with E2H set, so what it left is in the other register layout:
  // everything is set from scratch, FP/SIMD traps off first, as UEFI's
  // exception entry saves the FP registers.
10:mrs  x12, cptr_el2
  mov   x1, #CPTR_EL2_NO_TRAPS
  msr   cptr_el2, x1
  isb
  mov   x27, x0
  BREADCRUMB 'r'
  mrs   x28, hcr_el2
  mrs   x9, sctlr_el2
  mrs   x10, tcr_el2
  mrs   x11, vbar_el2
  mrs   x13, cntvoff_el2
  mrs   x14, daif
  mrs   x0, spsel
  orr   x14, x14, x0
  mov   x15, sp

  // UEFI's vectors, its stack, and the MMU and data cache off whatever
  // Gunyah left.
  msr   vbar_el2, x24
  MOV64 (x0, SCTLR_EL2_OFF)
  msr   sctlr_el2, x0
  isb
  msr   spsel, #1
  mov   sp, x20
  MOV64 (x0, HCR_EL2_HOST)
  msr   hcr_el2, x0
  isb

  // The timers and the counter as SEC leaves them at EL2.
  msr   cntvoff_el2, xzr
  msr   cnthp_ctl_el2, xzr
  msr   CNTHV_CTL_EL2, xzr
  msr   cntv_ctl_el0, xzr
  msr   cntp_ctl_el0, xzr
  isb

  // UEFI's EL1 tables for the EL2 regime.
  msr   mair_el2, x23
  mov   x0, #TCR_EL1_KEPT
  and   x0, x22, x0
  ubfx  x1, x22, #32, #3          // IPS -> PS
  orr   x0, x0, x1, lsl #16
  MOV64 (x1, TCR_EL2_RES1)
  orr   x0, x0, x1
  msr   tcr_el2, x0
  MOV64 (x1, TTBR_BADDR_MASK)
  and   x0, x21, x1
  msr   ttbr0_el2, x0
  isb
  tlbi  alle2is
  tlbi  alle1is
  dsb   ish
  isb
  BREADCRUMB 't'

  // Drop whatever is cached from before, then the MMU and caches on.
  bl    GunyahExitCleanCaches
  ic    iallu
  dsb   sy
  isb
  MOV64 (x0, SCTLR_EL2_ON)
  msr   sctlr_el2, x0
  isb
  BREADCRUMB 'm'

  // Memory is safe again.
  mov   x0, #2
  str   x0, [x19, #GUNYAH_EXIT_CURRENT_EL]
  str   x27, [x19, #GUNYAH_EXIT_RESULT]
  str   x28, [x19, #GUNYAH_EXIT_LEFT_HCR]
  str   x9, [x19, #GUNYAH_EXIT_LEFT_SCTLR]
  str   x10, [x19, #GUNYAH_EXIT_LEFT_TCR]
  str   x11, [x19, #GUNYAH_EXIT_LEFT_VBAR]
  str   x12, [x19, #GUNYAH_EXIT_LEFT_CPTR]
  str   x13, [x19, #GUNYAH_EXIT_LEFT_CNTVOFF]
  str   x14, [x19, #GUNYAH_EXIT_LEFT_DAIF]
  str   x15, [x19, #GUNYAH_EXIT_LEFT_SP]
  mrs   x0, tcr_el2
  str   x0, [x19, #GUNYAH_EXIT_EL2_TCR]
  mrs   x0, isr_el1
  str   x0, [x19, #GUNYAH_EXIT_ISR]

20:mov  x0, x27
  ldp   x27, x28, [sp, #80]
  ldp   x25, x26, [sp, #64]
  ldp   x23, x24, [sp, #48]
  ldp   x21, x22, [sp, #32]
  ldp   x19, x20, [sp, #16]
  ldp   x29, x30, [sp], #96
  ret
GunyahExitTextEnd:
