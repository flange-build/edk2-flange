#
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

#include <AsmMacroLib.h>
#include <AArch64/AArch64.h>

//
// EL1 exception vectors for Qcs6490EarlyInit(), which runs before SEC
// installs its own. Any exception is reported on the serial console with
// its syndrome, return address and fault address, and the CPU then parks:
// nothing this early can be recovered from, and a report beats a silent
// hang or a crash dump.
//
// ArmPlatformPkg's PeiVectorTable is not used: only SEC has it, and this
// library is linked into PEIMs as well.
//
// The handler runs on the temporary stack Qcs6490EarlyInit() used, reset to
// its top.
//

#define EARLY_STACK_TOP  (FixedPcdGet64 (PcdCPUCoresStackBase) + FixedPcdGet32 (PcdCPUCorePrimaryStackSize))

#define EARLY_VECTOR(Offset)           \
  .org    Offset                     ; \
  mov     x0, #(Offset)              ; \
  b       EarlyExceptionEntry

  .section .text.Qcs6490EarlyVectorTable, "ax"
  .balign 0x800
GCC_ASM_EXPORT(Qcs6490EarlyVectorTable)
ASM_PFX(Qcs6490EarlyVectorTable):
  EARLY_VECTOR(ARM_VECTOR_CUR_SP0_SYNC)
  EARLY_VECTOR(ARM_VECTOR_CUR_SP0_IRQ)
  EARLY_VECTOR(ARM_VECTOR_CUR_SP0_FIQ)
  EARLY_VECTOR(ARM_VECTOR_CUR_SP0_SERR)
  EARLY_VECTOR(ARM_VECTOR_CUR_SPX_SYNC)
  EARLY_VECTOR(ARM_VECTOR_CUR_SPX_IRQ)
  EARLY_VECTOR(ARM_VECTOR_CUR_SPX_FIQ)
  EARLY_VECTOR(ARM_VECTOR_CUR_SPX_SERR)
  EARLY_VECTOR(ARM_VECTOR_LOW_A64_SYNC)
  EARLY_VECTOR(ARM_VECTOR_LOW_A64_IRQ)
  EARLY_VECTOR(ARM_VECTOR_LOW_A64_FIQ)
  EARLY_VECTOR(ARM_VECTOR_LOW_A64_SERR)
  EARLY_VECTOR(ARM_VECTOR_LOW_A32_SYNC)
  EARLY_VECTOR(ARM_VECTOR_LOW_A32_IRQ)
  EARLY_VECTOR(ARM_VECTOR_LOW_A32_FIQ)
  EARLY_VECTOR(ARM_VECTOR_LOW_A32_SERR)
  .org    0x800

// x0 holds the vector offset.
EarlyExceptionEntry:
  MOV64 (x1, EARLY_STACK_TOP)
  mov   sp, x1
  mov   x29, xzr
  mrs   x1, esr_el1
  mrs   x2, elr_el1
  mrs   x3, far_el1
  mrs   x4, spsr_el1
  bl    ASM_PFX(Qcs6490EarlyException)
0:wfi
  b     0b

AARCH64_BTI_NOTE()
