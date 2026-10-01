/** @file
  DPU 7.2 (sc7280) part of the QCS6490 MDSS display driver: scans a linear
  XRGB8888 framebuffer out to INTF_1, which feeds DSI0.

  Data path: SSPP DMA0 -> LM_0 (blend stage 0, black border) -> PINGPONG_0 ->
  INTF_1, all driven by CTL_0 in video mode. PINGPONG_0 has no registers of
  its own on DPU 7.x apart from the dither block.

  The pipe is DMA0 rather than VIG0, which the vendor kernel happened to use
  for the golden register dump (its first primary plane). Both fetch through
  the same source pipe registers, QoS and CDP logic, but VIG0 also has a
  QSEED3 scaler and a CSC at +0xA00/+0x1A00 that the kernel programs for
  every frame and that the golden dump does not cover. DMA0 has neither, so
  every register it needs is in its first 0x1F8 bytes, and the values are
  those of the golden VIG0 dump. The pipe-specific parts differ: DMA0 is
  VBIF client 1, its forced-clock bit is TOP 0x2AC bit 8, and it has its own
  CTL layer field, fetch-active bit and flush bit.

  The register writes, their order and their values follow the vendor Linux
  kernel 6.6 (drivers/gpu/drm/msm/disp/dpu1: dpu_plane.c, dpu_crtc.c,
  dpu_encoder_phys_vid.c, dpu_encoder.c and the dpu_hw_*.c blocks), checked
  against the registers dumped from that kernel scanning out 1920x1080@60
  through this path.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"

//
// Bring-up aid (recipe section 8, PLAN.md M4): build with
// -DDPU_BORDER_FILL_ONLY=1 to stage no pipe and scan out a blue LM_0 border
// instead of the framebuffer. That tests the DSI link, the LT9611 and the
// INTF timing without any memory fetch.
//
#ifndef DPU_BORDER_FILL_ONLY
#define DPU_BORDER_FILL_ONLY  0
#endif

//
// Register blocks. Offsets of the DPU blocks are from the "mdp" region
// (catalog/dpu_7_2_sc7280.h); VBIF_RT is its own region (sc7280.dtsi).
//
#define DPU_MDP_BASE        0x0AE01000
#define DPU_TOP_BASE        (DPU_MDP_BASE + 0x00000)
#define DPU_CTL0_BASE       (DPU_MDP_BASE + 0x15000)
#define DPU_SSPP_DMA0_BASE  (DPU_MDP_BASE + 0x24000)
#define DPU_INTF1_BASE      (DPU_MDP_BASE + 0x35000)
#define DPU_LM0_BASE        (DPU_MDP_BASE + 0x44000)
#define DPU_PP0_BASE        (DPU_MDP_BASE + 0x69000)
#define DPU_VBIF_RT_BASE    0x0AEB0000

//
// MDP TOP (dpu_hwio.h, dpu_hw_top.c).
//
#define MDP_HW_VERSION                  0x000
#define MDP_HW_VERSION_DPU_7_2          0x7002          // bits 31:16
#define INTR_STATUS                     0x014           // raw, set even when not enabled
#define INTR_CLEAR                      0x018
#define INTR_INTF1_UNDERRUN             BIT26           // intr_underrun of INTF_1
#define INTR_INTF1_VSYNC                BIT27           // intr_vsync of INTF_1
#define SSPP_SPARE                      0x028
#define MDP_CLK_CTRL_DMA0               0x2AC           // clk_ctrls[DPU_CLK_CTRL_DMA0]
#define MDP_CLK_CTRL_DMA0_FORCE_ON      BIT8
#define SPLIT_DISPLAY_EN                0x2F4
#define SPLIT_DISPLAY_UPPER_PIPE_CTRL   0x2F8
#define DANGER_STATUS                   0x360
#define SAFE_STATUS                     0x364
#define SPLIT_DISPLAY_LOWER_PIPE_CTRL   0x3F0

//
// SSPP (dpu_hw_sspp.c, dpu_hw_util.c).
//
#define SSPP_SRC_SIZE                    0x000
#define SSPP_SRC_XY                      0x008
#define SSPP_OUT_SIZE                    0x00C
#define SSPP_OUT_XY                      0x010
#define SSPP_SRC0_ADDR                   0x014
#define SSPP_SRC1_ADDR                   0x018
#define SSPP_SRC2_ADDR                   0x01C
#define SSPP_SRC3_ADDR                   0x020
#define SSPP_SRC_YSTRIDE0                0x024
#define SSPP_SRC_YSTRIDE1                0x028
#define SSPP_SRC_FORMAT                  0x030
#define SSPP_SRC_UNPACK_PATTERN          0x034
#define SSPP_SRC_OP_MODE                 0x038
#define SSPP_DANGER_LUT                  0x060
#define SSPP_SAFE_LUT                    0x064
#define SSPP_QOS_CTRL                    0x06C
#define SSPP_CREQ_LUT_0                  0x074
#define SSPP_CREQ_LUT_1                  0x078
#define SSPP_SW_PIX_EXT_C0_LR            0x100
#define SSPP_SW_PIX_EXT_C0_TB            0x104
#define SSPP_SW_PIX_EXT_C0_REQ_PIXELS    0x108
#define SSPP_SW_PIX_EXT_C1C2_LR          0x110
#define SSPP_SW_PIX_EXT_C1C2_TB          0x114
#define SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS  0x118
#define SSPP_SW_PIX_EXT_C3_LR            0x120
#define SSPP_SW_PIX_EXT_C3_TB            0x124
#define SSPP_SW_PIX_EXT_C3_REQ_PIXELS    0x128
#define SSPP_CDP_CNTL                    0x134
#define SSPP_UBWC_ERROR_STATUS           0x138
#define SSPP_MULTIRECT_OPMODE            0x170

//
// SRC_FORMAT/UNPACK_PATTERN of DRM_FORMAT_XRGB8888 (dpu_formats.c): four
// 8-bit components, 4 bytes per pixel, unpacked tight, alpha not used,
// elements B, G, R, X in memory order. That is the UEFI
// PixelBlueGreenRedReserved8BitPerColor layout.
//
#define SSPP_SRC_FORMAT_XRGB8888    0x000236FF
#define SSPP_UNPACK_XRGB8888        0x03020001
#define SSPP_BYTES_PER_PIXEL        4

#define MDSS_MDP_OP_BWC_EN          BIT0
#define MDSS_MDP_OP_FLIP_LR         BIT13
#define MDSS_MDP_OP_FLIP_UD         BIT14
#define MDSS_MDP_OP_PE_OVERRIDE     BIT31
#define UBWC_ERROR_STATUS_CLEAR     BIT31

//
// QoS of a real-time linear pipe (sc7280_perf_data: danger_lut_tbl,
// safe_lut_tbl and sc7180_qos_macrotile for DPU_QOS_LUT_USAGE_LINEAR).
//
#define SSPP_DANGER_LUT_RT_LINEAR   0x0000FFFF
#define SSPP_SAFE_LUT_RT_LINEAR     0x0000FF00
#define SSPP_CREQ_LUT_RT_LINEAR_LO  0x44556677
#define SSPP_CREQ_LUT_RT_LINEAR_HI  0x00112233
#define QOS_CTRL_DANGER_SAFE_EN     BIT0
#define CDP_ENABLE                  BIT0
#define CDP_PRELOAD_AHEAD_64        BIT3

//
// LM (dpu_hw_lm.c). Blend stage 0 is at +0x20 (sc7180_lm_sblk).
//
#define LM_OP_MODE                  0x000
#define LM_OUT_SIZE                 0x004
#define LM_BORDER_COLOR_0           0x008           // G [11:0], B [27:16]
#define LM_BORDER_COLOR_1           0x010           // R [11:0], A [27:16]
#define LM_BLEND0_OP                0x020
#define LM_BLEND0_CONST_ALPHA       0x024
#define LM_OP_MODE_SPLIT_RIGHT      BIT31
#define LM_OP_MODE_KEEP_MASK        BIT30           // kept by setup_alpha_out
#define LM_OP_MODE_STAGE0_EN        BIT1            // 1 << DPU_STAGE_0
#define LM_BLEND_FG_ALPHA_FG_CONST  0
#define LM_BLEND_BG_ALPHA_BG_CONST  BIT8
#define LM_CONST_ALPHA_FG_OPAQUE    (0xFF << 16)
#define LM_BORDER_COMPONENT_MAX     0xFFF

//
// PINGPONG dither block (sc7280_pp_sblk, dpu_hw_pingpong.c).
//
#define PP_DITHER_BASE              0x0E0
#define PP_DITHER_EN                0x000

//
// CTL (dpu_hw_ctl.c). CTL_0 knows the mixers LM_0, LM_2 and LM_3.
//
#define CTL_LAYER(lm)               ((lm) * 0x4)
#define CTL_LAYER_EXT(lm)           (0x040 + (lm) * 0x4)
#define CTL_LAYER_EXT2(lm)          (0x070 + (lm) * 0x4)
#define CTL_LAYER_EXT3(lm)          (0x0A0 + (lm) * 0x4)
#define CTL_TOP                     0x014
#define CTL_FLUSH                   0x018
#define CTL_START                   0x01C
#define CTL_SW_RESET                0x030
#define CTL_INTF_ACTIVE             0x0F4
#define CTL_FETCH_PIPE_ACTIVE       0x0FC
#define CTL_INTF_FLUSH              0x110

#define CTL_MIXER_BORDER_OUT        BIT24
#define CTL_LAYER_DMA0_SHIFT        18              // ctl_blend_config[SSPP_DMA0]
#define CTL_LAYER_STAGE0_MIX        2               // (DPU_STAGE_0 + 1) & 7
#define CTL_TOP_GROUP_ID_DISABLED   (0xFU << 28)    // DPU_CTL_VM_CFG, video mode
#define CTL_FETCH_DMA0              BIT0            // fetch_tbl[SSPP_DMA0]
#define CTL_INTF_ACTIVE_INTF1       BIT1            // BIT (INTF_1 - INTF_0)
#define CTL_INTF_FLUSH_INTF1        BIT1
#define CTL_FLUSH_DMA0              BIT11
#define CTL_FLUSH_LM0               BIT6
#define CTL_FLUSH_CTL               BIT17
#define CTL_FLUSH_INTF              BIT31           // INTF_IDX: CTL_INTF_FLUSH applies
#define CTL_SW_RESET_BUSY           BIT0
#define CTL_START_KICKOFF           BIT0
#define CTL_RESET_TIMEOUT_US        2000            // DPU_REG_RESET_TIMEOUT_US

//
// INTF (dpu_hw_intf.c).
//
#define INTF_TIMING_ENGINE_EN       0x000
#define INTF_CONFIG                 0x004
#define INTF_HSYNC_CTL              0x008
#define INTF_VSYNC_PERIOD_F0        0x00C
#define INTF_VSYNC_PULSE_WIDTH_F0   0x014
#define INTF_DISPLAY_V_START_F0     0x01C
#define INTF_DISPLAY_V_END_F0       0x024
#define INTF_ACTIVE_V_START_F0      0x02C
#define INTF_ACTIVE_V_END_F0        0x034
#define INTF_DISPLAY_HCTL           0x03C
#define INTF_ACTIVE_HCTL            0x040
#define INTF_BORDER_COLOR           0x044
#define INTF_UNDERFLOW_COLOR        0x048
#define INTF_HSYNC_SKEW             0x04C
#define INTF_POLARITY_CTL           0x050
#define INTF_CONFIG2                0x060
#define INTF_DISPLAY_DATA_HCTL      0x064
#define INTF_ACTIVE_DATA_HCTL       0x068
#define INTF_PANEL_FORMAT           0x090
#define INTF_FRAME_LINE_COUNT_EN    0x0A8
#define INTF_FRAME_COUNT            0x0AC
#define INTF_LINE_COUNT             0x0B0
#define INTF_MUX                    0x25C
#define INTF_STATUS                 0x26C

#define INTF_CFG_ACTIVE_H_EN        BIT29
#define INTF_CFG_ACTIVE_V_EN        BIT30
#define INTF_CFG_PROG_FETCH_EN      BIT31
#define INTF_CFG2_DATA_HCTL_EN      BIT4
#define INTF_UNDERFLOW_COLOR_DEF    0xFF            // drm_mode_to_intf_timing_params
#define INTF_FRAME_LINE_COUNT_ON    0x3             // frame and line counters
#define INTF_MUX_PP_MASK            0xF
#define INTF_MUX_PP0                0x0             // PINGPONG_0 - PINGPONG_0
#define INTF_MUX_PP_NONE            0xF
#define INTF_STATUS_EN              BIT0
#define INTF_FRAME_COUNT_MASK       0xFFFF          // compare modulo 2^16

//
// PANEL_FORMAT for RGB888 out: 8 bits (COLOR_8BIT = 3) for G, B and R in
// bits 5:0, 0x21 in bits 15:8. Only 0x2100 reads back.
//
#define INTF_PANEL_FORMAT_RGB888    0x0000213F

//
// Programmable fetch is needed only when the vertical back porch and sync
// are shorter than this many lines (prog_fetch_lines_worst_case).
//
#define INTF_PROG_FETCH_LINES_WORST_CASE  24

//
// VBIF (dpu_hw_vbif.c, sdm845_vbif in dpu_hw_catalog.c).
//
#define VBIF_VERSION                  0x000
#define VBIF_OUT_AXI_AMEMTYPE_CONF0   0x160
#define VBIF_OUT_AXI_AMEMTYPE_CONF1   0x164
#define VBIF_XIN_PND_ERR              0x190
#define VBIF_XIN_SRC_ERR              0x194
#define VBIF_XIN_CLR_ERR              0x19C
#define VBIF_XIN_HALT_CTRL1           0x204
#define VBIF_XINL_QOS_RP_REMAP_000    0x550
#define VBIF_XINL_QOS_LVL_REMAP_000   0x590         // 0x550 + qos_rp_remap_size

//
// Memory type 3 for the 14 clients (4 bits each, 3 used), as
// dpu_vbif_init_memtypes sets it.
//
#define VBIF_AMEMTYPE_CONF0_MASK      0x77777777
#define VBIF_AMEMTYPE_CONF0_VALUE     0x33333333
#define VBIF_AMEMTYPE_CONF1_MASK      0x00777777
#define VBIF_AMEMTYPE_CONF1_VALUE     0x00333333

#define VBIF_XIN_DMA0                 1             // xin_id of SSPP_DMA0
#define VBIF_QOS_LEVELS               8
#define VBIF_QOS_REMAP_SHIFT(xin)     (((xin) & 0x7) * 4)
#define VBIF_QOS_REMAP_MASK           0x7

//
// VBIF_XIN_HALT_CTRL1 bits 29:16 read 1 for each client with nothing
// outstanding: 0x3FFF0000 in the golden dump with the display idle,
// 0x3FFE0000 while VIG0 (client 0) fetched. Used for logging only.
//
#define VBIF_XIN_IDLE(xin)            (BIT16 << (xin))

//
// Polling. Frames at 60 Hz last 16.7 ms.
//
#define DPU_FRAME_POLL_US             100
#define DPU_START_TIMEOUT_US          100000        // for the frames below
#define DPU_START_FRAMES              2             // measured after the first
#define DPU_FRAME_MARGIN_US           1000
#define DPU_STOP_FLUSH_TIMEOUT_US     20000
#define DPU_STOP_IDLE_TIMEOUT_US      1000
#define DPU_MAX_LINE_WIDTH            2400          // sc7280_dpu_caps.max_linewidth

typedef enum {
  DpuStateOff,
  DpuStateSetUp,
  DpuStateRunning
} DPU_STATE;

STATIC DPU_STATE  mDpuState;
STATIC UINT32     mDpuFrameUs;
STATIC UINT32     mDpuFlushMask;

//
// Priority remap of a real-time client (sdm845_rt_pri_lvl).
//
STATIC CONST UINT8  mVbifRtPriorityLevel[VBIF_QOS_LEVELS] = {
  3, 3, 4, 4, 5, 5, 6, 6
};

//
// The mixers whose stages CTL_0 clears (sc7280_lm: LM_0, LM_2, LM_3).
//
STATIC CONST UINT8  mCtlMixers[] = { 0, 2, 3 };

/**
  Checks that the mode and the framebuffer fit what the registers can hold.

  @param[in]  Timing           The mode.
  @param[in]  FrameBufferBase  The framebuffer.
  @param[in]  StrideBytes      Bytes per line.

  @retval EFI_SUCCESS            They fit.
  @retval EFI_INVALID_PARAMETER  A parameter is out of range.
  @retval EFI_UNSUPPORTED        The mode is too wide for one pipe.
**/
STATIC
EFI_STATUS
DpuCheckMode (
  IN CONST DISPLAY_TIMING  *Timing,
  IN EFI_PHYSICAL_ADDRESS  FrameBufferBase,
  IN UINT32                StrideBytes
  )
{
  UINT64  End;

  if ((Timing == NULL) || (Timing->HActive == 0) || (Timing->VActive == 0) ||
      (Timing->HSyncWidth == 0) || (Timing->VSyncWidth == 0) ||
      (Timing->PixelClockKhz == 0))
  {
    DEBUG ((DEBUG_ERROR, "%a: invalid timing\n", __func__));
    return EFI_INVALID_PARAMETER;
  }

  //
  // One pipe and one mixer: no source split, so no wider than a pipe line.
  //
  if (Timing->HActive > DPU_MAX_LINE_WIDTH) {
    DEBUG ((DEBUG_ERROR, "%a: %u pixels per line need two pipes\n", __func__, Timing->HActive));
    return EFI_UNSUPPORTED;
  }

  //
  // HSYNC_CTL and DISPLAY_HCTL hold the line length in 16 bits.
  //
  if (DISPLAY_H_TOTAL (Timing) > MAX_UINT16) {
    DEBUG ((DEBUG_ERROR, "%a: line of %u pixels too long\n", __func__, DISPLAY_H_TOTAL (Timing)));
    return EFI_UNSUPPORTED;
  }

  //
  // The source address registers are 32 bits wide, and the plane 0 pitch
  // takes the low 16 bits of SRC_YSTRIDE0.
  //
  End = FrameBufferBase + MultU64x32 (StrideBytes, Timing->VActive);
  if ((FrameBufferBase == 0) || (End > SIZE_4GB) ||
      (StrideBytes < (UINT32)Timing->HActive * SSPP_BYTES_PER_PIXEL) ||
      (StrideBytes > MAX_UINT16))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: framebuffer 0x%lx stride %u not usable\n",
      __func__,
      FrameBufferBase,
      StrideBytes
      ));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Reads the VBIF error bits of all clients, logs and clears them, as
  dpu_vbif_clear_errors does on every CRTC flush. An error here after
  scan-out has started means a fetch failed, typically an SMMU fault.

  @param[in]  When  Where in bring-up this is, for the log.
**/
STATIC
VOID
DpuClearVbifErrors (
  IN CONST CHAR8  *When
  )
{
  UINT32  Pending;
  UINT32  Source;

  Pending = MmioRead32 (DPU_VBIF_RT_BASE + VBIF_XIN_PND_ERR);
  Source  = MmioRead32 (DPU_VBIF_RT_BASE + VBIF_XIN_SRC_ERR);
  if ((Pending | Source) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a: VBIF client errors, pending 0x%x source 0x%x\n",
      __func__,
      When,
      Pending,
      Source
      ));
    MmioWrite32 (DPU_VBIF_RT_BASE + VBIF_XIN_CLR_ERR, Pending | Source);
  }
}

/**
  Sets up the VBIF the DPU fetches through: the AXI memory type of every
  client (dpu_vbif_init_memtypes, once at start-up) and the QoS priority
  remap of the DMA0 client (dpu_vbif_set_qos_remap, on the first commit of a
  real-time plane).

  sdm845_vbif has no default OT limit, so Linux writes no OT limit either.
**/
STATIC
VOID
DpuSetupVbif (
  VOID
  )
{
  UINT32   ClkCtrl;
  BOOLEAN  ForcedOn;
  UINTN    Level;
  UINT32   Shift;
  UINTN    Offset;

  MmioAndThenOr32 (
    DPU_VBIF_RT_BASE + VBIF_OUT_AXI_AMEMTYPE_CONF0,
    ~(UINT32)VBIF_AMEMTYPE_CONF0_MASK,
    VBIF_AMEMTYPE_CONF0_VALUE
    );
  MmioAndThenOr32 (
    DPU_VBIF_RT_BASE + VBIF_OUT_AXI_AMEMTYPE_CONF1,
    ~(UINT32)VBIF_AMEMTYPE_CONF1_MASK,
    VBIF_AMEMTYPE_CONF1_VALUE
    );

  DpuClearVbifErrors ("setup");

  //
  // Linux forces the pipe clock on around the remap writes, as the pipe is
  // clock gated while it does not fetch. (The golden dump stops at VBIF
  // 0x300, so these values come from the catalog only.)
  //
  ClkCtrl  = MmioRead32 (DPU_TOP_BASE + MDP_CLK_CTRL_DMA0);
  ForcedOn = (ClkCtrl & MDP_CLK_CTRL_DMA0_FORCE_ON) == 0;
  if (ForcedOn) {
    MmioWrite32 (DPU_TOP_BASE + MDP_CLK_CTRL_DMA0, ClkCtrl | MDP_CLK_CTRL_DMA0_FORCE_ON);
  }

  Shift = VBIF_QOS_REMAP_SHIFT (VBIF_XIN_DMA0);
  for (Level = 0; Level < VBIF_QOS_LEVELS; Level++) {
    //
    // Clients 8-15 are in the second word of each level.
    //
    Offset = ((VBIF_XIN_DMA0 & 0x8) >> 1) + Level * 8;
    MmioAndThenOr32 (
      DPU_VBIF_RT_BASE + VBIF_XINL_QOS_RP_REMAP_000 + Offset,
      ~((UINT32)VBIF_QOS_REMAP_MASK << Shift),
      (UINT32)mVbifRtPriorityLevel[Level] << Shift
      );
    MmioAndThenOr32 (
      DPU_VBIF_RT_BASE + VBIF_XINL_QOS_LVL_REMAP_000 + Offset,
      ~((UINT32)VBIF_QOS_REMAP_MASK << Shift),
      (UINT32)mVbifRtPriorityLevel[Level] << Shift
      );
  }

  if (ForcedOn) {
    MmioAnd32 (DPU_TOP_BASE + MDP_CLK_CTRL_DMA0, ~(UINT32)MDP_CLK_CTRL_DMA0_FORCE_ON);
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: memtype 0x%08x 0x%08x, QoS remap level 0/7 0x%08x/0x%08x\n",
    __func__,
    MmioRead32 (DPU_VBIF_RT_BASE + VBIF_OUT_AXI_AMEMTYPE_CONF0),
    MmioRead32 (DPU_VBIF_RT_BASE + VBIF_OUT_AXI_AMEMTYPE_CONF1),
    MmioRead32 (DPU_VBIF_RT_BASE + VBIF_XINL_QOS_RP_REMAP_000),
    MmioRead32 (DPU_VBIF_RT_BASE + VBIF_XINL_QOS_RP_REMAP_000 + (VBIF_QOS_LEVELS - 1) * 8)
    ));
}

/**
  Sets up SSPP DMA0 to fetch the whole framebuffer, unscaled, as
  dpu_plane_sspp_update_pipe does for a full-screen XRGB8888 plane: source
  address and pitch, rectangles, pixel extension, single-rectangle mode,
  format, CDP and QoS.

  @param[in]  Timing           The mode.
  @param[in]  FrameBufferBase  The framebuffer, below 4 GiB.
  @param[in]  StrideBytes      Bytes per line.
**/
STATIC
VOID
DpuSetupSspp (
  IN CONST DISPLAY_TIMING  *Timing,
  IN EFI_PHYSICAL_ADDRESS  FrameBufferBase,
  IN UINT32                StrideBytes
  )
{
  UINTN   Sspp;
  UINT32  Size;

  Sspp = DPU_SSPP_DMA0_BASE;
  Size = ((UINT32)Timing->VActive << 16) | Timing->HActive;

  MmioWrite32 (Sspp + SSPP_SRC0_ADDR, (UINT32)FrameBufferBase);
  MmioWrite32 (Sspp + SSPP_SRC1_ADDR, 0);
  MmioWrite32 (Sspp + SSPP_SRC2_ADDR, 0);
  MmioWrite32 (Sspp + SSPP_SRC3_ADDR, 0);
  MmioWrite32 (Sspp + SSPP_SRC_YSTRIDE0, StrideBytes);
  MmioWrite32 (Sspp + SSPP_SRC_YSTRIDE1, 0);

  MmioWrite32 (Sspp + SSPP_SRC_SIZE, Size);
  MmioWrite32 (Sspp + SSPP_SRC_XY, 0);
  MmioWrite32 (Sspp + SSPP_OUT_SIZE, Size);
  MmioWrite32 (Sspp + SSPP_OUT_XY, 0);

  //
  // No scaling: every component fetches exactly the source size, with no
  // pixels repeated or fetched beyond the edges. (Linux writes the C3 LR
  // value into C3_TB as well; both are 0.)
  //
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C0_LR, 0);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C0_TB, 0);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C0_REQ_PIXELS, Size);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C1C2_LR, 0);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C1C2_TB, 0);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS, Size);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C3_LR, 0);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C3_TB, 0);
  MmioWrite32 (Sspp + SSPP_SW_PIX_EXT_C3_REQ_PIXELS, Size);

  MmioWrite32 (Sspp + SSPP_MULTIRECT_OPMODE, 0);

  MmioWrite32 (Sspp + SSPP_SRC_FORMAT, SSPP_SRC_FORMAT_XRGB8888);
  MmioWrite32 (Sspp + SSPP_SRC_UNPACK_PATTERN, SSPP_UNPACK_XRGB8888);
  MmioAndThenOr32 (
    Sspp + SSPP_SRC_OP_MODE,
    ~(UINT32)(MDSS_MDP_OP_FLIP_LR | MDSS_MDP_OP_FLIP_UD | MDSS_MDP_OP_BWC_EN),
    MDSS_MDP_OP_PE_OVERRIDE
    );
  MmioWrite32 (Sspp + SSPP_UBWC_ERROR_STATUS, UBWC_ERROR_STATUS_CLEAR);

  MmioWrite32 (Sspp + SSPP_CDP_CNTL, CDP_PRELOAD_AHEAD_64 | CDP_ENABLE);

  //
  // A real-time pipe raises its priority as its buffer runs low, and the
  // DPU signals danger and safe levels to the memory system.
  //
  MmioWrite32 (Sspp + SSPP_DANGER_LUT, SSPP_DANGER_LUT_RT_LINEAR);
  MmioWrite32 (Sspp + SSPP_SAFE_LUT, SSPP_SAFE_LUT_RT_LINEAR);
  MmioWrite32 (Sspp + SSPP_CREQ_LUT_0, SSPP_CREQ_LUT_RT_LINEAR_LO);
  MmioWrite32 (Sspp + SSPP_CREQ_LUT_1, SSPP_CREQ_LUT_RT_LINEAR_HI);
  MmioWrite32 (Sspp + SSPP_QOS_CTRL, QOS_CTRL_DANGER_SAFE_EN);

  DEBUG ((
    DEBUG_INFO,
    "%a: DMA0 addr 0x%08x stride 0x%x size 0x%08x format 0x%08x unpack 0x%08x op 0x%08x\n",
    __func__,
    MmioRead32 (Sspp + SSPP_SRC0_ADDR),
    MmioRead32 (Sspp + SSPP_SRC_YSTRIDE0),
    MmioRead32 (Sspp + SSPP_SRC_SIZE),
    MmioRead32 (Sspp + SSPP_SRC_FORMAT),
    MmioRead32 (Sspp + SSPP_SRC_UNPACK_PATTERN),
    MmioRead32 (Sspp + SSPP_SRC_OP_MODE)
    ));
}

/**
  Sets up LM_0 to output the full mode with the pipe opaque at blend stage 0
  over a black border (_dpu_crtc_blend_setup), and CTL_0 to stage and fetch
  the pipe on it (clear_all_blendstages, set_active_pipes, setup_blendstage).

  @param[in]  Timing  The mode.
**/
STATIC
VOID
DpuSetupMixer (
  IN CONST DISPLAY_TIMING  *Timing
  )
{
  UINTN   Index;
  UINT32  Mixer;
  UINT32  Layer;
  UINT32  Fetch;
  UINT32  Stages;
  UINT32  BorderColor0;
  UINT32  BorderColor1;

  for (Index = 0; Index < ARRAY_SIZE (mCtlMixers); Index++) {
    Mixer = mCtlMixers[Index];
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER (Mixer), 0);
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT (Mixer), 0);
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT2 (Mixer), 0);
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT3 (Mixer), 0);
  }

  MmioWrite32 (DPU_CTL0_BASE + CTL_FETCH_PIPE_ACTIVE, 0);

  //
  // Linux leaves the border at its reset value, black. The border only
  // shows where no layer covers the mixer, which the full-screen layer does
  // everywhere. With no layer, no blend stage is enabled either, as Linux
  // leaves LM_0 when its plane is off.
  //
  BorderColor0 = 0;
  BorderColor1 = 0;
  Layer        = CTL_MIXER_BORDER_OUT;
  Fetch        = 0;
  Stages       = 0;
  if (DPU_BORDER_FILL_ONLY) {
    BorderColor0 = (UINT32)LM_BORDER_COMPONENT_MAX << 16;
    BorderColor1 = (UINT32)LM_BORDER_COMPONENT_MAX << 16;
  } else {
    Layer |= CTL_LAYER_STAGE0_MIX << CTL_LAYER_DMA0_SHIFT;
    Fetch  = CTL_FETCH_DMA0;
    Stages = LM_OP_MODE_STAGE0_EN;
  }

  MmioWrite32 (DPU_LM0_BASE + LM_BORDER_COLOR_0, BorderColor0);
  MmioWrite32 (DPU_LM0_BASE + LM_BORDER_COLOR_1, BorderColor1);

  //
  // XRGB8888 has no alpha: constant, fully opaque foreground.
  //
  MmioWrite32 (DPU_LM0_BASE + LM_BLEND0_CONST_ALPHA, LM_CONST_ALPHA_FG_OPAQUE);
  MmioWrite32 (DPU_LM0_BASE + LM_BLEND0_OP, LM_BLEND_FG_ALPHA_FG_CONST | LM_BLEND_BG_ALPHA_BG_CONST);

  MmioWrite32 (DPU_CTL0_BASE + CTL_FETCH_PIPE_ACTIVE, Fetch);

  MmioWrite32 (DPU_LM0_BASE + LM_OUT_SIZE, ((UINT32)Timing->VActive << 16) | Timing->HActive);
  MmioAnd32 (DPU_LM0_BASE + LM_OP_MODE, ~(UINT32)LM_OP_MODE_SPLIT_RIGHT);
  MmioAndThenOr32 (DPU_LM0_BASE + LM_OP_MODE, LM_OP_MODE_KEEP_MASK, Stages);

  MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER (0), Layer);
  MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT (0), 0);
  MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT2 (0), 0);
  MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT3 (0), 0);

  DEBUG ((
    DEBUG_INFO,
    "%a: LM_0 op 0x%x out 0x%08x blend 0x%x alpha 0x%08x, CTL_0 layer 0x%08x fetch 0x%x%a\n",
    __func__,
    MmioRead32 (DPU_LM0_BASE + LM_OP_MODE),
    MmioRead32 (DPU_LM0_BASE + LM_OUT_SIZE),
    MmioRead32 (DPU_LM0_BASE + LM_BLEND0_OP),
    MmioRead32 (DPU_LM0_BASE + LM_BLEND0_CONST_ALPHA),
    MmioRead32 (DPU_CTL0_BASE + CTL_LAYER (0)),
    MmioRead32 (DPU_CTL0_BASE + CTL_FETCH_PIPE_ACTIVE),
    DPU_BORDER_FILL_ONLY ? " (border fill only)" : ""
    ));
}

/**
  Sets up the INTF_1 timing generator for a mode, with the timing engine
  off, then connects CTL_0 and PINGPONG_0 to INTF_1: what
  dpu_encoder_phys_vid_enable does (split display off, setup_timing_gen,
  setup_intf_cfg, bind_pingpong_blk, programmable fetch).

  @param[in]  Timing  The mode.
**/
STATIC
VOID
DpuSetupIntf (
  IN CONST DISPLAY_TIMING  *Timing
  )
{
  UINTN   Intf;
  UINT32  HsyncPeriod;
  UINT32  VsyncPeriod;
  UINT32  HsyncStartX;
  UINT32  HsyncEndX;
  UINT32  DisplayVStart;
  UINT32  DisplayVEnd;
  UINT32  DisplayHctl;

  Intf = DPU_INTF1_BASE;

  //
  // A single DSI interface: no split display.
  //
  MmioWrite32 (DPU_TOP_BASE + SSPP_SPARE, 0);
  MmioWrite32 (DPU_TOP_BASE + SPLIT_DISPLAY_LOWER_PIPE_CTRL, 0);
  MmioWrite32 (DPU_TOP_BASE + SPLIT_DISPLAY_UPPER_PIPE_CTRL, 0);
  MmioWrite32 (DPU_TOP_BASE + SPLIT_DISPLAY_EN, 0);

  //
  // The counters run in pixel clocks from the start of the vertical sync
  // pulse; a line starts with the horizontal sync pulse. No hsync skew. The
  // image fills the mode, so there is no border fill in the INTF (active
  // window registers 0), and the DSI interface takes no sync polarity: the
  // sync pulses are always active high towards DSI, whatever the mode says.
  //
  HsyncPeriod   = DISPLAY_H_TOTAL (Timing);
  VsyncPeriod   = DISPLAY_V_TOTAL (Timing);
  HsyncStartX   = (UINT32)Timing->HSyncWidth + Timing->HBackPorch;
  HsyncEndX     = HsyncPeriod - Timing->HFrontPorch - 1;
  DisplayVStart = ((UINT32)Timing->VSyncWidth + Timing->VBackPorch) * HsyncPeriod;
  DisplayVEnd   = (VsyncPeriod - Timing->VFrontPorch) * HsyncPeriod - 1;
  DisplayHctl   = (HsyncEndX << 16) | HsyncStartX;

  MmioWrite32 (Intf + INTF_HSYNC_CTL, (HsyncPeriod << 16) | Timing->HSyncWidth);
  MmioWrite32 (Intf + INTF_VSYNC_PERIOD_F0, VsyncPeriod * HsyncPeriod);
  MmioWrite32 (Intf + INTF_VSYNC_PULSE_WIDTH_F0, (UINT32)Timing->VSyncWidth * HsyncPeriod);
  MmioWrite32 (Intf + INTF_DISPLAY_HCTL, DisplayHctl);
  MmioWrite32 (Intf + INTF_DISPLAY_V_START_F0, DisplayVStart);
  MmioWrite32 (Intf + INTF_DISPLAY_V_END_F0, DisplayVEnd);
  MmioWrite32 (Intf + INTF_ACTIVE_HCTL, 0);
  MmioWrite32 (Intf + INTF_ACTIVE_V_START_F0, 0);
  MmioWrite32 (Intf + INTF_ACTIVE_V_END_F0, 0);
  MmioWrite32 (Intf + INTF_BORDER_COLOR, 0);
  MmioWrite32 (Intf + INTF_UNDERFLOW_COLOR, INTF_UNDERFLOW_COLOR_DEF);
  MmioWrite32 (Intf + INTF_HSYNC_SKEW, 0);
  MmioWrite32 (Intf + INTF_POLARITY_CTL, 0);
  MmioWrite32 (Intf + INTF_FRAME_LINE_COUNT_EN, INTF_FRAME_LINE_COUNT_ON);

  //
  // INTF_CONFIG is read-modify-write in Linux; its other bits keep their
  // reset value (bit 23 reads 1). The active window stays off.
  //
  MmioAnd32 (Intf + INTF_CONFIG, ~(UINT32)(INTF_CFG_ACTIVE_H_EN | INTF_CFG_ACTIVE_V_EN));
  MmioWrite32 (Intf + INTF_PANEL_FORMAT, INTF_PANEL_FORMAT_RGB888);

  //
  // Data timing the same as the video timing: no compression, no wide bus.
  //
  MmioWrite32 (Intf + INTF_CONFIG2, INTF_CFG2_DATA_HCTL_EN);
  MmioWrite32 (Intf + INTF_DISPLAY_DATA_HCTL, DisplayHctl);
  MmioWrite32 (Intf + INTF_ACTIVE_DATA_HCTL, 0);

  //
  // CTL_0 drives INTF_1 in video mode. CTL_TOP[31:28] is the VM group ID,
  // which is not disabled at reset.
  //
  MmioWrite32 (DPU_CTL0_BASE + CTL_TOP, CTL_TOP_GROUP_ID_DISABLED);
  MmioOr32 (DPU_CTL0_BASE + CTL_INTF_ACTIVE, CTL_INTF_ACTIVE_INTF1);

  MmioAndThenOr32 (Intf + INTF_MUX, ~(UINT32)INTF_MUX_PP_MASK, INTF_MUX_PP0);

  //
  // The DSI pingpong has no dither to do for 8 bits per colour.
  //
  MmioWrite32 (DPU_PP0_BASE + PP_DITHER_BASE + PP_DITHER_EN, 0);

  //
  // The frame is fetched in the vertical back porch and sync. Fetching
  // earlier, in the front porch, is only needed when those are shorter than
  // the worst-case fetch latency.
  //
  if ((UINT32)Timing->VSyncWidth + Timing->VBackPorch < INTF_PROG_FETCH_LINES_WORST_CASE) {
    DEBUG ((
      DEBUG_WARN,
      "%a: vertical sync + back porch of %u lines is short, fetch may underrun\n",
      __func__,
      (UINT32)Timing->VSyncWidth + Timing->VBackPorch
      ));
  }

  MmioAnd32 (Intf + INTF_CONFIG, ~(UINT32)INTF_CFG_PROG_FETCH_EN);

  DEBUG ((
    DEBUG_INFO,
    "%a: INTF_1 hsync 0x%08x vperiod 0x%08x vpulse 0x%x hctl 0x%08x vstart 0x%x vend 0x%x\n",
    __func__,
    MmioRead32 (Intf + INTF_HSYNC_CTL),
    MmioRead32 (Intf + INTF_VSYNC_PERIOD_F0),
    MmioRead32 (Intf + INTF_VSYNC_PULSE_WIDTH_F0),
    MmioRead32 (Intf + INTF_DISPLAY_HCTL),
    MmioRead32 (Intf + INTF_DISPLAY_V_START_F0),
    MmioRead32 (Intf + INTF_DISPLAY_V_END_F0)
    ));
  DEBUG ((
    DEBUG_INFO,
    "%a: INTF_1 config 0x%08x config2 0x%x format 0x%x data hctl 0x%08x mux 0x%08x, CTL_0 top 0x%08x intf 0x%x\n",
    __func__,
    MmioRead32 (Intf + INTF_CONFIG),
    MmioRead32 (Intf + INTF_CONFIG2),
    MmioRead32 (Intf + INTF_PANEL_FORMAT),
    MmioRead32 (Intf + INTF_DISPLAY_DATA_HCTL),
    MmioRead32 (Intf + INTF_MUX),
    MmioRead32 (DPU_CTL0_BASE + CTL_TOP),
    MmioRead32 (DPU_CTL0_BASE + CTL_INTF_ACTIVE)
    ));
}

/**
  Turns the INTF_1 timing engine off and waits for the frame in progress to
  end, the way dpu_encoder_phys_vid_disable waits for up to two vsyncs.

  @param[in]  FrameUs  The frame period.
**/
STATIC
VOID
DpuStopTimingEngine (
  IN UINT32  FrameUs
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (DPU_INTF1_BASE + INTF_TIMING_ENGINE_EN, 0);

  //
  // The engine finishes the frame it is in; wait longer than a frame, then
  // for the status to confirm.
  //
  MicroSecondDelay (FrameUs + DPU_FRAME_MARGIN_US);
  Status = MmioPoll32 (DPU_INTF1_BASE + INTF_STATUS, INTF_STATUS_EN, 0, 2 * FrameUs);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: INTF_1 still running, status 0x%x frame %u\n",
      __func__,
      MmioRead32 (DPU_INTF1_BASE + INTF_STATUS),
      MmioRead32 (DPU_INTF1_BASE + INTF_FRAME_COUNT)
      ));
  }
}

/**
  Stops scan-out and leaves CTL_0 with nothing staged, fetched or connected,
  as dpu_encoder_phys_vid_disable and dpu_encoder_helper_phys_cleanup do:
  timing engine off, wait for the frame to end, CTL reset, blend stages and
  fetch cleared, INTF_1 unbound from PINGPONG_0 and inactive, then flush and
  start so that it takes effect without a vsync.

  Linux stages the border alone after clearing the stages and clears them
  again right after, before any flush; only the end result is written here.
**/
STATIC
VOID
DpuStopScanOut (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  UINT32      Mixer;
  UINT32      FrameCount;

  FrameCount = MmioRead32 (DPU_INTF1_BASE + INTF_FRAME_COUNT);

  if (((MmioRead32 (DPU_INTF1_BASE + INTF_STATUS) & INTF_STATUS_EN) != 0) ||
      (MmioRead32 (DPU_INTF1_BASE + INTF_TIMING_ENGINE_EN) != 0))
  {
    DpuStopTimingEngine (mDpuFrameUs);
  }

  MmioWrite32 (DPU_CTL0_BASE + CTL_SW_RESET, CTL_SW_RESET_BUSY);
  Status = MmioPoll32 (DPU_CTL0_BASE + CTL_SW_RESET, CTL_SW_RESET_BUSY, 0, CTL_RESET_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: CTL_0 reset does not complete\n", __func__));
  }

  for (Index = 0; Index < ARRAY_SIZE (mCtlMixers); Index++) {
    Mixer = mCtlMixers[Index];
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER (Mixer), 0);
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT (Mixer), 0);
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT2 (Mixer), 0);
    MmioWrite32 (DPU_CTL0_BASE + CTL_LAYER_EXT3 (Mixer), 0);
  }

  MmioWrite32 (DPU_CTL0_BASE + CTL_FETCH_PIPE_ACTIVE, 0);

  MmioAndThenOr32 (DPU_INTF1_BASE + INTF_MUX, ~(UINT32)INTF_MUX_PP_MASK, INTF_MUX_PP_NONE);
  MmioWrite32 (DPU_PP0_BASE + PP_DITHER_BASE + PP_DITHER_EN, 0);
  MmioAnd32 (DPU_CTL0_BASE + CTL_INTF_ACTIVE, ~(UINT32)CTL_INTF_ACTIVE_INTF1);

  MmioWrite32 (DPU_CTL0_BASE + CTL_INTF_FLUSH, CTL_INTF_FLUSH_INTF1);
  MmioWrite32 (DPU_CTL0_BASE + CTL_FLUSH, CTL_FLUSH_INTF | CTL_FLUSH_CTL | CTL_FLUSH_LM0);
  MmioWrite32 (DPU_CTL0_BASE + CTL_START, CTL_START_KICKOFF);

  //
  // Read-backs only: Linux does not wait for this flush.
  //
  Status = MmioPoll32 (DPU_CTL0_BASE + CTL_FLUSH, MAX_UINT32, 0, DPU_STOP_FLUSH_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: CTL_0 flush 0x%x still pending\n",
      __func__,
      MmioRead32 (DPU_CTL0_BASE + CTL_FLUSH)
      ));
  }

  //
  // The DMA0 client should have nothing outstanding any more before the
  // SMMU stops translating for it.
  //
  Status = MmioPoll32 (
             DPU_VBIF_RT_BASE + VBIF_XIN_HALT_CTRL1,
             VBIF_XIN_IDLE (VBIF_XIN_DMA0),
             VBIF_XIN_IDLE (VBIF_XIN_DMA0),
             DPU_STOP_IDLE_TIMEOUT_US
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: VBIF client %u not idle, halt ctrl 0x%08x\n",
      __func__,
      VBIF_XIN_DMA0,
      MmioRead32 (DPU_VBIF_RT_BASE + VBIF_XIN_HALT_CTRL1)
      ));
  }

  DpuClearVbifErrors ("stop");

  //
  // Leave no raw INTF_1 status of ours behind for the OS.
  //
  MmioWrite32 (DPU_TOP_BASE + INTR_CLEAR, INTR_INTF1_UNDERRUN | INTR_INTF1_VSYNC);

  DEBUG ((
    DEBUG_INFO,
    "%a: stopped after frame %u: INTF_1 status 0x%x, CTL_0 layer 0x%x fetch 0x%x intf 0x%x flush 0x%x\n",
    __func__,
    FrameCount,
    MmioRead32 (DPU_INTF1_BASE + INTF_STATUS),
    MmioRead32 (DPU_CTL0_BASE + CTL_LAYER (0)),
    MmioRead32 (DPU_CTL0_BASE + CTL_FETCH_PIPE_ACTIVE),
    MmioRead32 (DPU_CTL0_BASE + CTL_INTF_ACTIVE),
    MmioRead32 (DPU_CTL0_BASE + CTL_FLUSH)
    ));

  mDpuState = DpuStateOff;
}

/**
  Waits for the INTF_1 frame counter to advance, and measures the frame
  period between the first and the last frame seen.

  @param[in]   Frames         The frames to see after the first.
  @param[in]   TimeoutUs      How long to wait, in microseconds.
  @param[out]  FramePeriodUs  The measured frame period.
  @param[out]  FramesSeen     How many frames went by.

  @retval EFI_SUCCESS  The counter advanced by Frames + 1.
  @retval EFI_TIMEOUT  It did not.
**/
STATIC
EFI_STATUS
DpuWaitForFrames (
  IN  UINT32  Frames,
  IN  UINTN   TimeoutUs,
  OUT UINT32  *FramePeriodUs,
  OUT UINT32  *FramesSeen
  )
{
  UINT32   Start;
  UINT32   First;
  UINT32   Count;
  UINT32   Measured;
  UINT64   FirstNs;
  UINT64   Ns;
  UINTN    Elapsed;
  BOOLEAN  Seen;

  Start          = MmioRead32 (DPU_INTF1_BASE + INTF_FRAME_COUNT);
  First          = Start;
  FirstNs        = 0;
  Seen           = FALSE;
  *FramePeriodUs = 0;
  *FramesSeen    = 0;

  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += DPU_FRAME_POLL_US) {
    MicroSecondDelay (DPU_FRAME_POLL_US);
    Count = MmioRead32 (DPU_INTF1_BASE + INTF_FRAME_COUNT);
    if (((Count - Start) & INTF_FRAME_COUNT_MASK) == 0) {
      continue;
    }

    *FramesSeen = (Count - Start) & INTF_FRAME_COUNT_MASK;
    Ns          = GetTimeInNanoSecond (GetPerformanceCounter ());
    if (!Seen) {
      Seen    = TRUE;
      First   = Count;
      FirstNs = Ns;
      continue;
    }

    Measured = (Count - First) & INTF_FRAME_COUNT_MASK;
    if (Measured >= Frames) {
      *FramePeriodUs = (UINT32)DivU64x32 (Ns - FirstNs, 1000 * Measured);
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

/**
  Sets up the DPU to scan out a linear XRGB8888 framebuffer to INTF_1 (DSI0),
  without starting the timing engine.

  @param[in]  Timing           The mode.
  @param[in]  FrameBufferBase  The framebuffer, below 4 GiB.
  @param[in]  StrideBytes      Bytes per line.

  @retval EFI_SUCCESS  Set up.
  @retval Other        Failed.
**/
EFI_STATUS
DpuSetup (
  IN CONST DISPLAY_TIMING    *Timing,
  IN EFI_PHYSICAL_ADDRESS    FrameBufferBase,
  IN UINT32                  StrideBytes
  )
{
  EFI_STATUS  Status;
  UINT32      Version;

  Status = DpuCheckMode (Timing, FrameBufferBase, StrideBytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Version = MmioRead32 (DPU_TOP_BASE + MDP_HW_VERSION);
  DEBUG ((
    DEBUG_INFO,
    "%a: DPU 0x%08x VBIF 0x%08x, INTF_1 status 0x%x, CTL_0 flush 0x%x\n",
    __func__,
    Version,
    MmioRead32 (DPU_VBIF_RT_BASE + VBIF_VERSION),
    MmioRead32 (DPU_INTF1_BASE + INTF_STATUS),
    MmioRead32 (DPU_CTL0_BASE + CTL_FLUSH)
    ));

  //
  // All offsets below are those of DPU 7.2.
  //
  if ((Version >> 16) != MDP_HW_VERSION_DPU_7_2) {
    DEBUG ((DEBUG_ERROR, "%a: not a DPU 7.2\n", __func__));
    return EFI_UNSUPPORTED;
  }

  mDpuFrameUs = (UINT32)DivU64x32 (
                          MultU64x32 ((UINT64)DISPLAY_H_TOTAL (Timing) * DISPLAY_V_TOTAL (Timing), 1000),
                          Timing->PixelClockKhz
                          ) + 1;

  //
  // Nothing should have started the display before, but reprogramming a
  // running timing engine would send broken frames.
  //
  if ((MmioRead32 (DPU_INTF1_BASE + INTF_STATUS) & INTF_STATUS_EN) != 0) {
    DEBUG ((DEBUG_WARN, "%a: INTF_1 already running, stopping it\n", __func__));
    DpuStopTimingEngine (mDpuFrameUs);
  }

  DpuSetupVbif ();
  DpuSetupSspp (Timing, FrameBufferBase, StrideBytes);
  DpuSetupMixer (Timing);
  DpuSetupIntf (Timing);

  mDpuFlushMask = CTL_FLUSH_INTF | CTL_FLUSH_CTL | CTL_FLUSH_LM0;
  if (!DPU_BORDER_FILL_ONLY) {
    mDpuFlushMask |= CTL_FLUSH_DMA0;
  }

  mDpuState = DpuStateSetUp;

  DEBUG ((
    DEBUG_INFO,
    "%a: %ux%u, frame %u us, framebuffer 0x%lx stride %u\n",
    __func__,
    Timing->HActive,
    Timing->VActive,
    mDpuFrameUs,
    FrameBufferBase,
    StrideBytes
    ));

  return EFI_SUCCESS;
}

/**
  Flushes the configuration and starts the INTF_1 timing engine, then checks
  that frames are being produced.

  @retval EFI_SUCCESS  The frame counter advances.
  @retval Other        It does not; scan-out has been stopped.
**/
EFI_STATUS
DpuStart (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      FramePeriodUs;
  UINT32      FramesSeen;
  UINT32      IntrStatus;
  UINT32      Flush;
  BOOLEAN     Again;

  if (mDpuState != DpuStateSetUp) {
    DEBUG ((DEBUG_ERROR, "%a: not set up\n", __func__));
    return EFI_NOT_READY;
  }

  //
  // A CTL reset the hardware started on its own must be over before the
  // flush (dpu_encoder_phys_vid_prepare_for_kickoff).
  //
  Status = MmioPoll32 (DPU_CTL0_BASE + CTL_SW_RESET, CTL_SW_RESET_BUSY, 0, CTL_RESET_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: CTL_0 stuck in reset\n", __func__));
    goto Fail;
  }

  //
  // Nothing takes interrupts here, but the raw status still latches: clear
  // what is left over so that an underrun seen below is ours.
  //
  MmioWrite32 (DPU_TOP_BASE + INTR_CLEAR, INTR_INTF1_UNDERRUN | INTR_INTF1_VSYNC);
  DpuClearVbifErrors ("start");

  //
  // In video mode the flushed configuration latches at the next vsync of the
  // interface, so the timing engine starts after the flush
  // (dpu_encoder_phys_vid_handle_post_kickoff).
  //
  MmioWrite32 (DPU_CTL0_BASE + CTL_INTF_FLUSH, CTL_INTF_FLUSH_INTF1);
  MmioWrite32 (DPU_CTL0_BASE + CTL_FLUSH, mDpuFlushMask);
  MmioWrite32 (DPU_INTF1_BASE + INTF_TIMING_ENGINE_EN, 1);
  mDpuState = DpuStateRunning;

  DEBUG ((DEBUG_INFO, "%a: flushed 0x%08x, timing engine on\n", __func__, mDpuFlushMask));

  Status = DpuWaitForFrames (DPU_START_FRAMES, DPU_START_TIMEOUT_US, &FramePeriodUs, &FramesSeen);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %u frames in %u ms (INTF_1 status 0x%x, line %u); is the pixel clock running?\n",
      __func__,
      FramesSeen,
      DPU_START_TIMEOUT_US / 1000,
      MmioRead32 (DPU_INTF1_BASE + INTF_STATUS),
      MmioRead32 (DPU_INTF1_BASE + INTF_LINE_COUNT)
      ));
    goto Fail;
  }

  //
  // The flush latched at the first vsync; Linux waits for CTL_FLUSH to read
  // 0 after a commit (wait_for_commit_done).
  //
  Flush = MmioRead32 (DPU_CTL0_BASE + CTL_FLUSH);
  if (Flush != 0) {
    DEBUG ((DEBUG_ERROR, "%a: CTL_0 flush 0x%08x not taken after %u frames\n", __func__, Flush, FramesSeen));
    goto Fail;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: running, frame %u, period %u us (%u mHz), line %u, VBIF halt ctrl 0x%08x\n",
    __func__,
    MmioRead32 (DPU_INTF1_BASE + INTF_FRAME_COUNT),
    FramePeriodUs,
    (FramePeriodUs != 0) ? (UINT32)DivU64x32 (1000000000, FramePeriodUs) : 0,
    MmioRead32 (DPU_INTF1_BASE + INTF_LINE_COUNT),
    MmioRead32 (DPU_VBIF_RT_BASE + VBIF_XIN_HALT_CTRL1)
    ));

  //
  // Not fatal: the link runs and shows the underflow colour where the pipe
  // could not fetch in time (bandwidth vote, SMMU, framebuffer address).
  // Tell an underrun that keeps coming back from one while starting.
  //
  IntrStatus = MmioRead32 (DPU_TOP_BASE + INTR_STATUS);
  if ((IntrStatus & INTR_INTF1_UNDERRUN) != 0) {
    MmioWrite32 (DPU_TOP_BASE + INTR_CLEAR, INTR_INTF1_UNDERRUN);
    MicroSecondDelay (2 * mDpuFrameUs);
    Again = (MmioRead32 (DPU_TOP_BASE + INTR_STATUS) & INTR_INTF1_UNDERRUN) != 0;
    DEBUG ((
      Again ? DEBUG_ERROR : DEBUG_WARN,
      "%a: INTF_1 underrun%a (interrupt status 0x%08x, danger 0x%08x, safe 0x%08x)\n",
      __func__,
      Again ? ", again in the next frames" : " while starting only",
      IntrStatus,
      MmioRead32 (DPU_TOP_BASE + DANGER_STATUS),
      MmioRead32 (DPU_TOP_BASE + SAFE_STATUS)
      ));
  }

  DpuClearVbifErrors ("running");

  return EFI_SUCCESS;

Fail:
  DpuStopScanOut ();
  return EFI_DEVICE_ERROR;
}

/**
  Stops scan-out: timing engine off, then, after the current frame, no pipe
  fetching and no layer staged. Leaves the DPU idle for the OS.
**/
VOID
DpuStop (
  VOID
  )
{
  if (mDpuState == DpuStateOff) {
    return;
  }

  DpuStopScanOut ();
}
