/** @file
  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#define TZ_EL2_SWITCH_SMC_ID              0x02000121
#define TZ_EL2_SWITCH_PARAM_ID            0x00000023
#define TZ_EL2_SWITCH_PARAM2_KEEP_GUNYAH  0x0
#define TZ_EL2_SWITCH_PARAM2_EXIT_GUNYAH  0x1

//
// The result asking for a preempted call to be resumed (x0 = 1), with the
// session in x6.
//
#define QCOM_SCM_INTERRUPTED  1
