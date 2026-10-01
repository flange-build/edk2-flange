#
#  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

#include <AsmMacroLib.h>
#include <Library/ArmLib.h>
#include "Qcs6490Helper.h"

//
// The top of the SEC primary stack, where Qcs6490EarlyInit() runs. SEC fills
// the whole stack with PcdInitValueInTempStack before it uses it.
//
#define EARLY_STACK_TOP  (FixedPcdGet64 (PcdCPUCoresStackBase) + FixedPcdGet32 (PcdCPUCorePrimaryStackSize))

ASM_FUNC(ArmPlatformPeiBootAction)
// Many Qualcomm targets use Gunyah as a firmware-launched hypervisor, making
// EL2 unavailable for Non-secure firmware and operating systems, preventing
// use of alternative hypervisors.
//
// XBL enters SEC at EL1 as a Gunyah guest, with the MMU and caches off and
// no stack. Qcs6490EarlyInit() decides whether to leave Gunyah: from the
// HypervisorMode variable, which it reads from the variable store on UFS,
// else from xbl_config's OsConfigTableSelection as the stock Qualcomm UEFI
// does, else from PcdExitGunyah. It runs on a temporary stack at the top of
// the SEC primary stack, with exception vectors of its own (EarlyVectors.S)
// that report a fault on the console; SEC sets up both anew afterwards.
//
// To leave Gunyah, use a TZ SMC call supported on these Qcom targets to
// clean up EL2/Gunyah configuration and transition to booting UEFI at EL2,
// as the upstream RB3 Gen 2 port does. All the SMMU configuration gets
// cleaned up and handed over in EL2 in bypass mode. This has to happen
// before SEC sets up the stack for the exception level it runs at.
//
// Otherwise UEFI stays a Gunyah guest at EL1, and ArmPlatformInitialize()
// confirms that with the same call once the console is up.
//
// Entered at EL2 already, there is nothing to decide.

  EL1_OR_EL2(x0)

// Only x10 and the return address are live in the caller. Keep the
// caller's x19-x21 and x29 on the temporary stack, and its stack pointer,
// the return address and the interrupt masks in x19-x21, which the C code
// preserves.
1:mov   x16, sp
  mov   x17, x30
  MOV64 (x9, EARLY_STACK_TOP)
  mov   sp, x9
  stp   x19, x20, [sp, #-32]!
  stp   x21, x29, [sp, #16]
  mov   x19, x16
  mov   x20, x17
  mrs   x21, daif
  msr   daifset, #0x3

// Report exceptions through our own vectors (they need 2 KiB alignment).
// They stay installed until SEC's CEntryPoint() installs its own.
  adrp  x0, ASM_PFX(Qcs6490EarlyVectorTable)
  add   x0, x0, :lo12:ASM_PFX(Qcs6490EarlyVectorTable)
  tst   x0, #0x7FF
  b.ne  4f
  msr   vbar_el1, x0
  isb

4:mov   x29, xzr
  bl    ASM_PFX(Qcs6490EarlyInit)
  and   w0, w0, #0xFF

  msr   daif, x21
  mov   x30, x20
  mov   x16, x19
  ldp   x21, x29, [sp, #16]
  ldp   x19, x20, [sp], #32
  mov   sp, x16
  cbz   w0, 3f

  ldr   w0, =TZ_EL2_SWITCH_SMC_ID
  mov   x6, xzr
0:ldr   w1, =TZ_EL2_SWITCH_PARAM_ID
  mov   w2, wzr
  mov   w3, wzr
  ldr   w4, =TZ_EL2_SWITCH_PARAM2_EXIT_GUNYAH
  mov   w5, wzr
  smc   #0
// A positive result means TZ was preempted: resume the call with the result
// as the function ID and the session TZ left in x6, like Qualcomm's own SCM
// callers do.
  cmp   w0, #0
  b.gt  0b

  ldr   x1, =ASM_PFX(gQcs6490ExitGunyahStatus)
  str   w0, [x1]

// Still at EL1 if TZ refused the call; carry on as a Gunyah guest.
  mrs   x0, CurrentEL
  cmp   x0, #0x8
  b.ne  3f

// At EL2 the timers are ours. Zero the virtual offset so that the virtual
// counter UEFI reads also matches what the EL1 virtual timer compares against
// when HCR_EL2.E2H is set, and silence the EL2 physical timer, whose PPI
// TimerDxe enables as well.
2:msr   cntvoff_el2, xzr
  msr   cnthp_ctl_el2, xzr
  isb

// SEC passes x10 on as the TransferList base, but only loads it when it finds
// one. XBL never hands one over, so drop whatever XBL, the early code or the
// SMC left in the register.
3:mov   x10, xzr
  ret

ASM_FUNCTION_REMOVE_IF_UNREFERENCED
