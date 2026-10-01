/** @file
  Lets UFS DMA through the QCS6490 apps SMMU for SEC, while UEFI is still a
  Gunyah guest.

  Under Gunyah the apps SMMU faults any stream the guest has not set up, and
  a faulting UFS transfer takes the system down for a crash dump. SEC sets
  the UFS stream up the way SmmuDxe does in DXE (see there): a stream match
  entry pointing the stream at the last context bank with translation
  disabled, since the hypervisor turns a BYPASS entry into FAULT. The
  hypervisor still applies its stage 2.

  Only the stream match and stream-to-context entries are put back
  afterwards, so that SmmuDxe finds the SMMU as SEC found it. The context
  bank stays as set up: the hypervisor aborts a guest that writes back the
  stage 2 type its CBAR reads with, and SmmuDxe sets the same bank up the
  same way.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/IoLib.h>

#include "Qcs6490Early.h"
#include "Qcs6490LibInternal.h"

//
// apps_smmu in kodiak.dtsi.
//
#define APPS_SMMU_BASE  0x15000000

//
// Arm SMMUv2 (MMU-500) registers.
//
#define SMMU_SCR0             0x000
#define SMMU_SCR0_EXIDENABLE  BIT3

#define SMMU_IDR0          0x020
#define SMMU_IDR0_SMS      BIT27
#define SMMU_IDR0_NUMSMRG  0xFF

#define SMMU_IDR1                 0x024
#define SMMU_IDR1_PAGESIZE        BIT31
#define SMMU_IDR1_NUMPAGENDXB(x)  (((x) >> 28) & 0x7)
#define SMMU_IDR1_NUMCB(x)        ((x) & 0xFF)

#define SMMU_SMR(n)          (APPS_SMMU_BASE + 0x800 + 4 * (n))
#define SMMU_SMR_VALID       BIT31
#define SMMU_SMR_MASK_SHIFT  16
#define SMMU_SMR_ID_MASK     0x7FFF
#define SMMU_SMR_EXID_MASK   0xFFFF

#define SMMU_S2CR(n)            (APPS_SMMU_BASE + 0xC00 + 4 * (n))
#define SMMU_S2CR_TYPE(x)       (((x) >> 16) & 0x3)
#define SMMU_S2CR_TYPE_TRANS    0
#define SMMU_S2CR_TYPE_BYPASS   1
#define SMMU_S2CR_TYPE_FAULT    2
#define SMMU_S2CR_TYPE_SHIFT    16
#define SMMU_S2CR_CBNDX(x)      ((x) & 0xFF)
#define SMMU_S2CR_EXIDVALID     BIT10
#define SMMU_S2CR_CBNDX_NONE    0xFF

#define SMMU_GR1_CBAR(n)                   (4 * (n))
#define SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS  (1 << 16)

#define SMMU_CB_SCTLR    0x000
#define SMMU_CB_SCTLR_M  BIT0

//
// The UFS stream: iommus of ufs_mem_hc in kodiak.dtsi.
//
#define UFS_STREAM_ID    0x80
#define UFS_STREAM_MASK  0x0

/**
  Returns whether a stream match entry is in use.

  @param[in]  Index        The entry.
  @param[in]  ExtendedIds  Whether the SMMU uses extended stream IDs.

  @retval TRUE   The entry matches a stream.
  @retval FALSE  The entry is free.
**/
STATIC
BOOLEAN
IsEntryValid (
  IN UINT32   Index,
  IN BOOLEAN  ExtendedIds
  )
{
  if (ExtendedIds) {
    return (MmioRead32 (SMMU_S2CR (Index)) & SMMU_S2CR_EXIDVALID) != 0;
  }

  return (MmioRead32 (SMMU_SMR (Index)) & SMMU_SMR_VALID) != 0;
}

/**
  Lets the UFS stream through the apps SMMU without stage 1 translation.

  @param[out] Smmu  What was changed, for Qcs6490EarlySmmuRestore ().

  @retval RETURN_SUCCESS  UFS may do DMA.
  @retval Other           It may not; nothing was changed.
**/
RETURN_STATUS
Qcs6490EarlySmmuEnableUfs (
  OUT QCS6490_EARLY_SMMU  *Smmu
  )
{
  UINT32   Idr0;
  UINT32   Idr1;
  UINT32   NumSmrg;
  UINT32   NumCb;
  UINT32   PageShift;
  UINTN    Gr1Base;
  UINTN    CbBase;
  BOOLEAN  ExtendedIds;
  UINT32   FieldMask;
  UINT32   Index;
  UINT32   Free;
  UINT32   Smr;
  UINT32   S2cr;
  UINT32   Cb;

  Smmu->Changed     = FALSE;
  Smmu->ExtendedIds = FALSE;
  Smmu->Index       = 0;
  Smmu->Smr         = 0;
  Smmu->S2cr        = 0;

  Idr0 = MmioRead32 (APPS_SMMU_BASE + SMMU_IDR0);
  Idr1 = MmioRead32 (APPS_SMMU_BASE + SMMU_IDR1);

  NumSmrg     = Idr0 & SMMU_IDR0_NUMSMRG;
  NumCb       = SMMU_IDR1_NUMCB (Idr1);
  PageShift   = ((Idr1 & SMMU_IDR1_PAGESIZE) != 0) ? 16 : 12;
  Gr1Base     = APPS_SMMU_BASE + ((UINTN)1 << PageShift);
  CbBase      = APPS_SMMU_BASE + ((UINTN)1 << (SMMU_IDR1_NUMPAGENDXB (Idr1) + 1 + PageShift));
  ExtendedIds = (MmioRead32 (APPS_SMMU_BASE + SMMU_SCR0) & SMMU_SCR0_EXIDENABLE) != 0;
  FieldMask   = ExtendedIds ? SMMU_SMR_EXID_MASK : SMMU_SMR_ID_MASK;

  if (((Idr0 & SMMU_IDR0_SMS) == 0) || (NumSmrg == 0) || (NumCb == 0)) {
    Qcs6490Print ("QCS6490: SMMU: no stream matching (IDR0 0x%x IDR1 0x%x)\n", Idr0, Idr1);
    return RETURN_UNSUPPORTED;
  }

  //
  // A second entry for a stream already matched would be a stream match
  // conflict. Use such an entry only if it lets the stream through
  // untranslated.
  //
  Free = NumSmrg;
  for (Index = 0; Index < NumSmrg; Index++) {
    if (!IsEntryValid (Index, ExtendedIds)) {
      if (Free == NumSmrg) {
        Free = Index;
      }

      continue;
    }

    Smr = MmioRead32 (SMMU_SMR (Index));
    if ((((Smr & FieldMask) ^ UFS_STREAM_ID) &
         ~(((Smr >> SMMU_SMR_MASK_SHIFT) & FieldMask) | UFS_STREAM_MASK) & FieldMask) != 0)
    {
      continue;
    }

    S2cr = MmioRead32 (SMMU_S2CR (Index));
    if ((SMMU_S2CR_TYPE (S2cr) == SMMU_S2CR_TYPE_BYPASS) ||
        ((SMMU_S2CR_TYPE (S2cr) == SMMU_S2CR_TYPE_TRANS) &&
         (SMMU_S2CR_CBNDX (S2cr) < NumCb) &&
         ((MmioRead32 (CbBase + ((UINTN)SMMU_S2CR_CBNDX (S2cr) << PageShift) + SMMU_CB_SCTLR) &
           SMMU_CB_SCTLR_M) == 0)))
    {
      Qcs6490Print (
        "QCS6490: SMMU: UFS stream 0x%x already in entry %u (SMR 0x%x S2CR 0x%x)\n",
        UFS_STREAM_ID,
        Index,
        Smr,
        S2cr
        );
      return RETURN_SUCCESS;
    }

    Qcs6490Print (
      "QCS6490: SMMU: UFS stream 0x%x translated by entry %u (SMR 0x%x S2CR 0x%x)\n",
      UFS_STREAM_ID,
      Index,
      Smr,
      S2cr
      );
    return RETURN_ACCESS_DENIED;
  }

  if (Free == NumSmrg) {
    Qcs6490Print ("QCS6490: SMMU: no free stream match entry for UFS\n");
    return RETURN_OUT_OF_RESOURCES;
  }

  Smmu->Index       = Free;
  Smmu->Smr         = MmioRead32 (SMMU_SMR (Free));
  Smmu->S2cr        = MmioRead32 (SMMU_S2CR (Free));
  Smmu->ExtendedIds = ExtendedIds;
  Smmu->Changed     = TRUE;

  //
  // Try BYPASS first; when the hypervisor refuses it, use the same context
  // bank Linux and SmmuDxe pick, with translation disabled.
  //
  Cb   = SMMU_S2CR_CBNDX_NONE;
  S2cr = (SMMU_S2CR_TYPE_BYPASS << SMMU_S2CR_TYPE_SHIFT) | SMMU_S2CR_CBNDX_NONE;
  MmioWrite32 (SMMU_S2CR (Free), S2cr);
  if (SMMU_S2CR_TYPE (MmioRead32 (SMMU_S2CR (Free))) != SMMU_S2CR_TYPE_BYPASS) {
    Cb = NumCb - 1;
    MmioWrite32 (CbBase + ((UINTN)Cb << PageShift) + SMMU_CB_SCTLR, 0);
    MmioWrite32 (Gr1Base + SMMU_GR1_CBAR (Cb), SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS);
    S2cr = (SMMU_S2CR_TYPE_TRANS << SMMU_S2CR_TYPE_SHIFT) | Cb;
  }

  //
  // Point the entry at its context first, as Linux does, then make it match
  // the stream.
  //
  Smr = UFS_STREAM_ID | ((UINT32)UFS_STREAM_MASK << SMMU_SMR_MASK_SHIFT);
  if (ExtendedIds) {
    MmioWrite32 (SMMU_S2CR (Free), S2cr);
    MmioWrite32 (SMMU_SMR (Free), Smr);
    MmioWrite32 (SMMU_S2CR (Free), S2cr | SMMU_S2CR_EXIDVALID);
  } else {
    MmioWrite32 (SMMU_S2CR (Free), S2cr);
    MmioWrite32 (SMMU_SMR (Free), Smr | SMMU_SMR_VALID);
  }

  ArmDataSynchronizationBarrier ();

  if (Cb == SMMU_S2CR_CBNDX_NONE) {
    Qcs6490Print ("QCS6490: SMMU: UFS stream 0x%x -> entry %u, bypass\n", UFS_STREAM_ID, Free);
  } else {
    Qcs6490Print (
      "QCS6490: SMMU: UFS stream 0x%x -> entry %u, context bank %u\n",
      UFS_STREAM_ID,
      Free,
      Cb
      );
  }

  return RETURN_SUCCESS;
}

/**
  Puts back the stream match entry Qcs6490EarlySmmuEnableUfs () used. UFS
  must not do DMA any more.

  @param[in, out] Smmu  What was changed.
**/
VOID
Qcs6490EarlySmmuRestore (
  IN OUT QCS6490_EARLY_SMMU  *Smmu
  )
{
  if (!Smmu->Changed) {
    return;
  }

  //
  // Put back the half that holds the valid bit first (SMR.VALID, or
  // S2CR.EXIDVALID with extended stream IDs), so that the entry is out of
  // use before its other half changes. The other way round, a still valid
  // entry would for a moment match the stream ID of the saved SMR.
  //
  if (Smmu->ExtendedIds) {
    MmioWrite32 (SMMU_S2CR (Smmu->Index), Smmu->S2cr);
    MmioWrite32 (SMMU_SMR (Smmu->Index), Smmu->Smr);
  } else {
    MmioWrite32 (SMMU_SMR (Smmu->Index), Smmu->Smr);
    MmioWrite32 (SMMU_S2CR (Smmu->Index), Smmu->S2cr);
  }

  ArmDataSynchronizationBarrier ();

  Smmu->Changed = FALSE;
}
