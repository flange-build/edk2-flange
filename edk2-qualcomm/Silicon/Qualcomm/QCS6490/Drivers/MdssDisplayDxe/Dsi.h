/** @file
  DSI0 host controller and DSI0 PHY: what Dsi.c and DsiPhy.c share.

  As in the Linux MSM DSI driver, the PHY is reset through a register of the
  DSI host, and the host takes the clock lane timing it programs (T_CLK_PRE,
  T_CLK_POST) from the PHY timing.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MDSS_DSI_H_
#define MDSS_DSI_H_

#include "MdssDisplay.h"

//
// DSI0 controller (mdss_dsi0 in sc7280.dtsi). A DSI 6G controller has its
// 6G_HW_VERSION register at offset 0, and every register of the DSI register
// map 4 bytes further (DSI_6G_REG_SHIFT in the Linux driver).
//
#define DSI0_CTRL_BASE       0x0AE94000
#define DSI_6G_REG_SHIFT     4
#define DSI0_REG(Offset)     (DSI0_CTRL_BASE + DSI_6G_REG_SHIFT + (Offset))

//
// The link to the LT9611: 4 data lanes (data-lanes <0 1 2 3>), RGB888.
//
#define DSI_LANE_COUNT       4
#define DSI_BITS_PER_PIXEL   24

//
// The clock lane timing of the D-PHY timing that the host also programs
// (struct msm_dsi_phy_shared_timings in Linux). Both are in units of 16 UI.
//
typedef struct {
  UINT8    ClkPre;
  UINT8    ClkPost;
} DSI_PHY_SHARED_TIMINGS;

/**
  Returns the clock lane timing the PHY uses for a mode, for the host's
  CLKOUT_TIMING_CTRL.

  @param[in]   Timing  The mode.
  @param[out]  Shared  The timing.

  @retval EFI_SUCCESS      Returned.
  @retval EFI_UNSUPPORTED  The PHY has no settings for the pixel clock.
**/
EFI_STATUS
DsiPhyGetSharedTimings (
  IN  CONST DISPLAY_TIMING    *Timing,
  OUT DSI_PHY_SHARED_TIMINGS  *Shared
  );

/**
  Checks that the DSI0 controller answers as the expected DSI 6G v2.5, then
  resets the DSI0 PHY through its DSI_PHY_RESET register, as the Linux DSI
  manager does before it enables the PHY.

  @retval EFI_SUCCESS       The PHY has been reset.
  @retval EFI_DEVICE_ERROR  The controller is not the expected one; nothing
                            was written.
**/
EFI_STATUS
DsiHostResetPhy (
  VOID
  );

#endif // MDSS_DSI_H_
