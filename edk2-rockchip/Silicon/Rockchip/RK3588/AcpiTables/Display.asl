/** @file
 *
 *  RK3588 display subsystem: VOP2 and its HDMI/DP transmitters.
 *
 *  The firmware brings the display up for GOP and leaves it running, so
 *  this device has no power methods: switching the VOP domain off while
 *  the OS still scans out of the GOP framebuffer would blank the screen.
 *
 *  Resources are listed in a fixed order that the OS driver relies on:
 *    Memory  0: VOP2 registers
 *    Memory  1: VOP2 gamma LUT
 *    Memory  2: VOP GRF
 *    Memory  3: VO0 GRF
 *    Memory  4: VO1 GRF
 *    Memory  5: HDMI TX0 (DW HDMI QP)
 *    Memory  6: HDMI TX1 (DW HDMI QP)
 *    Memory  7: DP TX0
 *    Memory  8: DP TX1
 *    Memory  9: HDPTX PHY0
 *    Memory 10: HDPTX PHY1
 *    Memory 11: HDPTX PHY0 GRF
 *    Memory 12: HDPTX PHY1 GRF
 *    Interrupt  0: VOP2
 *    Interrupt  1-5: HDMI TX0 avp, cec, earc, main, hpd
 *    Interrupt  6-10: HDMI TX1 avp, cec, earc, main, hpd
 *    Interrupt 11: DP TX0
 *    Interrupt 12: DP TX1
 *
 *  Clocks and resets (CRU, PMU1CRU) are not listed: those registers are
 *  shared with every other block and use write masks, so the driver maps
 *  them itself.
 *
 *  Boards list the outputs that are actually wired up in
 *  BOARD_DISPLAY_OUTPUTS before including this file.
 *
 *  Copyright (c) 2026, Eric Wu <eric3u@outlook.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "AcpiTables.h"

#ifndef BOARD_DISPLAY_OUTPUTS
#define BOARD_DISPLAY_OUTPUTS  "hdmi0", "hdmi1", "dp0", "dp1"
#endif

Device (VOP0) {
  Name (_HID, "RKCP0E10")
  Name (_UID, 0)
  Name (_CCA, 0)
  Name (_STA, 0xF)

  Name (_CRS, ResourceTemplate () {
    Memory32Fixed (ReadWrite, 0xFDD90000, 0x4200)   // VOP2
    Memory32Fixed (ReadWrite, 0xFDD95000, 0x1000)   // VOP2 gamma LUT
    Memory32Fixed (ReadWrite, 0xFD5A4000, 0x2000)   // VOP GRF
    Memory32Fixed (ReadWrite, 0xFD5A6000, 0x2000)   // VO0 GRF
    Memory32Fixed (ReadWrite, 0xFD5A8000, 0x4000)   // VO1 GRF
    Memory32Fixed (ReadWrite, 0xFDE80000, 0x20000)  // HDMI TX0
    Memory32Fixed (ReadWrite, 0xFDEA0000, 0x20000)  // HDMI TX1
    Memory32Fixed (ReadWrite, 0xFDE50000, 0x4000)   // DP TX0
    Memory32Fixed (ReadWrite, 0xFDE60000, 0x4000)   // DP TX1
    Memory32Fixed (ReadWrite, 0xFED60000, 0x2000)   // HDPTX PHY0
    Memory32Fixed (ReadWrite, 0xFED70000, 0x2000)   // HDPTX PHY1
    Memory32Fixed (ReadWrite, 0xFD5E0000, 0x100)    // HDPTX PHY0 GRF
    Memory32Fixed (ReadWrite, 0xFD5E4000, 0x100)    // HDPTX PHY1 GRF

    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 188 }  // VOP2
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 201 }  // HDMI0 avp
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 202 }  // HDMI0 cec
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 203 }  // HDMI0 earc
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 204 }  // HDMI0 main
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 392 }  // HDMI0 hpd
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 205 }  // HDMI1 avp
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 206 }  // HDMI1 cec
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 207 }  // HDMI1 earc
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 208 }  // HDMI1 main
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 393 }  // HDMI1 hpd
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 193 }  // DP0
    Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 194 }  // DP1
  })

  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "rockchip,rk3588-vop" },
      Package () { "rockchip,outputs", Package () { BOARD_DISPLAY_OUTPUTS } },
    }
  })
}
