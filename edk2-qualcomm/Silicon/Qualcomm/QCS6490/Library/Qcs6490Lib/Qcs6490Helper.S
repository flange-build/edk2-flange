#
#  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

#include <AsmMacroLib.h>
#include <Library/ArmLib.h>
#include "Qcs6490Helper.h"

ASM_FUNC(ArmPlatformPeiBootAction)
// Many Qualcomm targets use Gunyah as a firmware-launched hypervisor, making
// EL2 unavailable for Non-secure firmware and operating systems, preventing
// use of alternative hypervisors.
//
// With PcdExitGunyah set, use a TZ SMC call supported on these Qcom targets
// to clean up EL2/Gunyah configuration and transition to booting UEFI at EL2
// exiting Gunyah, as the upstream RB3 Gen 2 port does. All the SMMU
// configuration gets cleaned up and handed over in EL2 in bypass mode. This
// has to happen before SEC sets up the stack for the exception level it runs
// at.
//
// Otherwise UEFI stays a Gunyah guest at EL1, and ArmPlatformInitialize()
// confirms that with the same call once the console is up.

  EL1_OR_EL2(x0)
1:ldr   x0, =ASM_PFX(gQcs6490ExitGunyah)
  ldrb  w0, [x0]
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
// one. XBL never hands one over, so drop whatever XBL, or the SMC, left in
// the register.
3:mov   x10, xzr
  ret

ASM_FUNCTION_REMOVE_IF_UNREFERENCED
