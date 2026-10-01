/** @file
  Lets the devices UEFI drives do DMA through the QCS6490 apps SMMU while UEFI
  runs as a Gunyah guest.

  Under Gunyah the apps SMMU faults any stream the guest has not set up. The
  first UFS command then faults the UFS stream, and the system goes down for
  a crash dump. The stock Qualcomm UEFI sets up an SMMU domain for UFS before
  using it; here the stream bypasses stage 1 instead, the way Linux
  (arm-smmu-qcom) sets up bypass under the Qualcomm hypervisor: the
  hypervisor turns a BYPASS stream-to-context entry into FAULT, so the stream
  goes to a context bank with translation disabled. The hypervisor still
  applies its stage 2.

  After TrustZone has removed Gunyah, the SMMU is left in bypass and UEFI's
  own DMA needs nothing. A remote processor UEFI starts keeps running into
  the OS, though, and Linux enables the SMMU with unmatched streams faulting.
  When the OS runs at EL2 this driver therefore offers QCS6490_SMMU_PROTOCOL,
  which hands such a stream over to the OS in bypass. When Gunyah leaves only
  at ExitBootServices (GunyahExitDxe), the streams are recorded, and set up
  once it has left: Gunyah wipes the entries it does not leave in bypass.

  The stream-to-context entries are put back at ExitBootServices, so the OS
  finds no stream matched that the boot firmware did not match. The display
  stops fetching before that, in the BeforeExitBootServices group, and
  XhciDxe halts the USB host controllers in an ExitBootServices handler of
  the same TPL that it creates later, so it runs first. The bypass
  context bank stays as it is: the hypervisor aborts a guest that gives a
  context bank the stage 2 type the bank reads back with, and Linux sets up
  the same context bank the same way anyway.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Qcs6490NvStatusLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/Qcs6490Smmu.h>
#include <Protocol/Qcs6490SmmuReady.h>

//
// apps_smmu in kodiak.dtsi.
//
#define APPS_SMMU_BASE  0x15000000

//
// Arm SMMUv2 (MMU-500) registers.
//
#define SMMU_SCR0             0x000
#define SMMU_SCR0_CLIENTPD    BIT0
#define SMMU_SCR0_EXIDENABLE  BIT3
#define SMMU_SCR0_USFCFG      BIT10

#define SMMU_IDR0          0x020
#define SMMU_IDR0_SMS      BIT27
#define SMMU_IDR0_EXIDS    BIT8
#define SMMU_IDR0_NUMSMRG  0xFF

#define SMMU_IDR1                 0x024
#define SMMU_IDR1_PAGESIZE        BIT31
#define SMMU_IDR1_NUMPAGENDXB(x)  (((x) >> 28) & 0x7)
#define SMMU_IDR1_NUMCB(x)        ((x) & 0xFF)

#define SMMU_SGFSR  0x048

#define SMMU_SMR(n)          (0x800 + 4 * (n))
#define SMMU_SMR_VALID       BIT31
#define SMMU_SMR_MASK_SHIFT  16
#define SMMU_SMR_ID_MASK     0x7FFF
#define SMMU_SMR_EXID_MASK   0xFFFF

#define SMMU_S2CR(n)              (0xC00 + 4 * (n))
#define SMMU_S2CR_TYPE(x)         (((x) >> 16) & 0x3)
#define SMMU_S2CR_TYPE_TRANS      0
#define SMMU_S2CR_TYPE_BYPASS     1
#define SMMU_S2CR_TYPE_SHIFT      16
#define SMMU_S2CR_EXIDVALID       BIT10
#define SMMU_S2CR_CBNDX_NONE      0xFF

#define SMMU_GR1_CBAR(n)                   (4 * (n))
#define SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS  (1 << 16)

#define SMMU_CB_SCTLR  0x000

typedef struct {
  UINT16         Id;
  UINT16         Mask;
  CONST CHAR8    *Name;
} SMMU_STREAM;

//
// iommus of the devices UEFI does DMA with, from kodiak.dtsi.
//
STATIC CONST SMMU_STREAM  mStreams[] = {
  { 0x80,   0x0,   "UFS"   },
  { 0x900,  0x402, "MDSS"  },
  { 0xa0,   0x0,   "USB2"  },
  { 0x1c00, 0x1,   "PCIE0" },   // the root port and 01:00.0 (iommu-map)
};

typedef struct {
  UINTN     Index;
  UINT32    Smr;
  UINT32    S2cr;
} SAVED_ENTRY;

STATIC SAVED_ENTRY  mSavedEntries[ARRAY_SIZE (mStreams)];
STATIC UINTN        mSavedEntryCount;

STATIC BOOLEAN  mBypassCbUsed;
STATIC UINTN    mBypassCb;

//
// Streams handed over while Gunyah is there, for after it has left.
//
#define SMMU_MAX_PENDING  4

STATIC SMMU_STREAM  mPending[SMMU_MAX_PENDING];
STATIC UINTN        mPendingCount;
STATIC BOOLEAN      mDeferred;

STATIC UINTN    mNumSmrg;
STATIC UINTN    mGr1Base;
STATIC UINTN    mCbBase;
STATIC UINTN    mPageShift;
STATIC BOOLEAN  mExtendedIds;

STATIC EFI_EVENT  mExitBootServicesEvent;

/**
  Returns whether a stream-to-context entry is in use.

  @param[in]  Index  The entry.

  @retval TRUE   The entry matches a stream.
  @retval FALSE  The entry is free.
**/
STATIC
BOOLEAN
IsEntryValid (
  IN UINTN  Index
  )
{
  if (mExtendedIds) {
    return (MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index)) & SMMU_S2CR_EXIDVALID) != 0;
  }

  return (MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index)) & SMMU_SMR_VALID) != 0;
}

/**
  Returns whether a valid entry already matches some of a stream's IDs, in
  which case a second entry would be a stream match conflict.

  @param[in]  NumSmrg  The number of entries.
  @param[in]  Stream   The stream.
  @param[out] Index    The entry that matches.

  @retval TRUE   An entry matches.
  @retval FALSE  None does.
**/
STATIC
BOOLEAN
FindOverlappingEntry (
  IN  UINTN              NumSmrg,
  IN  CONST SMMU_STREAM  *Stream,
  OUT UINTN              *Index
  )
{
  UINT32  Smr;
  UINT32  FieldMask;
  UINT32  Id;
  UINT32  Mask;

  FieldMask = mExtendedIds ? SMMU_SMR_EXID_MASK : SMMU_SMR_ID_MASK;

  for (*Index = 0; *Index < NumSmrg; (*Index)++) {
    if (!IsEntryValid (*Index)) {
      continue;
    }

    Smr  = MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (*Index));
    Id   = Smr & FieldMask;
    Mask = (Smr >> SMMU_SMR_MASK_SHIFT) & FieldMask;
    if (((Id ^ Stream->Id) & ~(Mask | Stream->Mask) & FieldMask) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Returns the S2CR value that bypasses stage 1, setting up the bypass context
  bank the first time the hypervisor refuses a BYPASS entry.

  @param[in]  Index   A free entry, to find out how the hypervisor treats
                      BYPASS.
  @param[in]  NumCb   The number of context banks.

  @return  The S2CR value, without EXIDVALID.
**/
STATIC
UINT32
GetBypassS2cr (
  IN UINTN  Index,
  IN UINTN  NumCb
  )
{
  UINT32  S2cr;

  if (mBypassCbUsed) {
    return (SMMU_S2CR_TYPE_TRANS << SMMU_S2CR_TYPE_SHIFT) | (UINT32)mBypassCb;
  }

  S2cr = (SMMU_S2CR_TYPE_BYPASS << SMMU_S2CR_TYPE_SHIFT) | SMMU_S2CR_CBNDX_NONE;
  MmioWrite32 (APPS_SMMU_BASE + SMMU_S2CR (Index), S2cr);
  if (SMMU_S2CR_TYPE (MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))) == SMMU_S2CR_TYPE_BYPASS) {
    return S2cr;
  }

  //
  // Same context bank as Linux picks, so that the OS takes over the same
  // arrangement.
  //
  mBypassCb = NumCb - 1;
  DEBUG ((
    DEBUG_INFO,
    "%a: BYPASS refused, using context bank %u (SCTLR 0x%x, CBAR 0x%x)\n",
    __func__,
    mBypassCb,
    MmioRead32 (mCbBase + (mBypassCb << mPageShift) + SMMU_CB_SCTLR),
    MmioRead32 (mGr1Base + SMMU_GR1_CBAR (mBypassCb))
    ));

  MmioWrite32 (mCbBase + (mBypassCb << mPageShift) + SMMU_CB_SCTLR, 0);
  MmioWrite32 (mGr1Base + SMMU_GR1_CBAR (mBypassCb), SMMU_CBAR_TYPE_S1_TRANS_S2_BYPASS);
  mBypassCbUsed = TRUE;

  return (SMMU_S2CR_TYPE_TRANS << SMMU_S2CR_TYPE_SHIFT) | (UINT32)mBypassCb;
}

/**
  Puts the stream-to-context entries back as the boot firmware left them.

  @param[in]  Event    The ExitBootServices event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
RestoreSmmu (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN  Index;

  for (Index = 0; Index < mSavedEntryCount; Index++) {
    MmioWrite32 (APPS_SMMU_BASE + SMMU_SMR (mSavedEntries[Index].Index), mSavedEntries[Index].Smr);
    MmioWrite32 (APPS_SMMU_BASE + SMMU_S2CR (mSavedEntries[Index].Index), mSavedEntries[Index].S2cr);
  }
}

/**
  Makes a stream bypass the SMMU, now and for the OS, at EL2.

  @param[in]  StreamId  The stream ID.
  @param[in]  Mask      The stream ID bits to ignore.
  @param[in]  Name      A name for the log.

  @retval EFI_SUCCESS            The stream bypasses the SMMU, now and for the
                                 OS.
  @retval EFI_ACCESS_DENIED      Another entry already matches the stream and
                                 does not bypass.
  @retval EFI_UNSUPPORTED        The SMMU uses extended stream IDs.
  @retval EFI_OUT_OF_RESOURCES   No stream match entry is free.
**/
STATIC
EFI_STATUS
HandOverNow (
  IN UINT16       StreamId,
  IN UINT16       Mask,
  IN CONST CHAR8  *Name
  )
{
  UINTN    Index;
  UINT32   Smr;
  UINT32   S2cr;
  UINT32   EntrySmr;
  UINT32   EntryId;
  UINT32   EntryMask;
  BOOLEAN  Covered;
  BOOLEAN  Conflict;

  //
  // Linux reads only the valid bit of the stream match register when it
  // adopts the boot firmware's entries.
  //
  if (mExtendedIds) {
    DEBUG ((DEBUG_ERROR, "%a: extended stream IDs enabled, %a stream 0x%x not handed over\n", __func__, Name, StreamId));
    return EFI_UNSUPPORTED;
  }

  Smr  = SMMU_SMR_VALID | ((UINT32)Mask << SMMU_SMR_MASK_SHIFT) | StreamId;
  S2cr = (SMMU_S2CR_TYPE_BYPASS << SMMU_S2CR_TYPE_SHIFT) | SMMU_S2CR_CBNDX_NONE;

  //
  // Every entry that matches some of the stream's IDs: two entries matching
  // one ID is a stream match conflict once Linux turns the SMMU on. A BYPASS
  // entry that matches all of them does the job already.
  //
  Covered  = FALSE;
  Conflict = FALSE;
  for (Index = 0; Index < mNumSmrg; Index++) {
    if (!IsEntryValid (Index)) {
      continue;
    }

    EntrySmr  = MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index));
    EntryId   = EntrySmr & SMMU_SMR_ID_MASK;
    EntryMask = (EntrySmr >> SMMU_SMR_MASK_SHIFT) & SMMU_SMR_ID_MASK;
    if (((EntryId ^ StreamId) & ~(EntryMask | Mask) & SMMU_SMR_ID_MASK) != 0) {
      continue;
    }

    if (((Mask & ~EntryMask) == 0) &&
        (SMMU_S2CR_TYPE (MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))) == SMMU_S2CR_TYPE_BYPASS))
    {
      DEBUG ((DEBUG_INFO, "%a: %a stream 0x%x already bypasses in entry %u (SMR 0x%x)\n", __func__, Name, StreamId, Index, EntrySmr));
      Covered = TRUE;
      continue;
    }

    DEBUG ((
      DEBUG_ERROR,
      "%a: %a stream 0x%x mask 0x%x also matched by entry %u (SMR 0x%x, S2CR 0x%x)\n",
      __func__,
      Name,
      StreamId,
      Mask,
      Index,
      EntrySmr,
      MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))
      ));
    Conflict = TRUE;
  }

  if (Conflict) {
    return EFI_ACCESS_DENIED;
  }

  if (Covered) {
    return EFI_SUCCESS;
  }

  //
  // The lowest free entry, below the one Linux writes to find out whether
  // BYPASS entries work: the last, or entry 127 when there are more than
  // 128 (arm-smmu-qcom qcom_smmu_cfg_probe).
  //
  for (Index = 0; Index < MIN (mNumSmrg - 1, 127) && IsEntryValid (Index); Index++) {
  }

  if (Index >= MIN (mNumSmrg - 1, 127)) {
    DEBUG ((DEBUG_ERROR, "%a: no free stream match group for %a\n", __func__, Name));
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // The context first, then the match, as Linux does.
  //
  MmioWrite32 (APPS_SMMU_BASE + SMMU_S2CR (Index), S2cr);
  MmioWrite32 (APPS_SMMU_BASE + SMMU_SMR (Index), Smr);
  ArmDataSynchronizationBarrier ();

  DEBUG ((
    DEBUG_INFO,
    "%a: %a stream 0x%x mask 0x%x: entry %u, SMR 0x%x, S2CR 0x%x, kept for the OS\n",
    __func__,
    Name,
    StreamId,
    Mask,
    Index,
    MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index)),
    MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))
    ));

  return EFI_SUCCESS;
}

/**
  Implements QCS6490_SMMU_PROTOCOL.HandOverBypass().

  @param[in]  This      The protocol.
  @param[in]  StreamId  The stream ID.
  @param[in]  Mask      The stream ID bits to ignore.
  @param[in]  Name      A name for the log.

  @retval EFI_SUCCESS            The stream bypasses the SMMU, now and for the
                                 OS, or will once Gunyah has left.
  @retval EFI_ACCESS_DENIED      Another entry already matches the stream and
                                 does not bypass.
  @retval EFI_UNSUPPORTED        The SMMU uses extended stream IDs.
  @retval EFI_OUT_OF_RESOURCES   No stream match entry is free.
**/
STATIC
EFI_STATUS
EFIAPI
HandOverBypass (
  IN QCS6490_SMMU_PROTOCOL  *This,
  IN UINT16                 StreamId,
  IN UINT16                 Mask,
  IN CONST CHAR8            *Name
  )
{
  //
  // Gunyah owns the SMMU until it leaves at ExitBootServices.
  //
  if (mDeferred && (ArmReadCurrentEL () == AARCH64_EL1)) {
    if (mPendingCount == ARRAY_SIZE (mPending)) {
      return EFI_OUT_OF_RESOURCES;
    }

    mPending[mPendingCount].Id   = StreamId;
    mPending[mPendingCount].Mask = Mask;
    mPending[mPendingCount].Name = Name;
    mPendingCount++;

    DEBUG ((DEBUG_INFO, "%a: %a stream 0x%x mask 0x%x, for once Gunyah has left\n", __func__, Name, StreamId, Mask));
    return EFI_SUCCESS;
  }

  return HandOverNow (StreamId, Mask, Name);
}

/**
  Logs the SMMU as Gunyah left it when it went: the global configuration and
  the stream match entries in use, which Linux adopts as bypass streams.
  Reads only.

  @param[in]  Idr0  IDR0.
  @param[in]  Idr1  IDR1.
**/
STATIC
VOID
LogEl2State (
  IN UINT32  Idr0,
  IN UINT32  Idr1
  )
{
  UINT32  Scr0;
  UINTN   Index;
  UINTN   Valid;

  Scr0 = MmioRead32 (APPS_SMMU_BASE + SMMU_SCR0);

  DEBUG ((
    DEBUG_INFO,
    "%a: EL2: sCR0 0x%x (CLIENTPD %u, USFCFG %u, EXIDENABLE %u), sGFSR 0x%x, IDR0 0x%x (EXIDS %u), IDR1 0x%x, %u stream match groups\n",
    __func__,
    Scr0,
    (Scr0 & SMMU_SCR0_CLIENTPD) != 0,
    (Scr0 & SMMU_SCR0_USFCFG) != 0,
    (Scr0 & SMMU_SCR0_EXIDENABLE) != 0,
    MmioRead32 (APPS_SMMU_BASE + SMMU_SGFSR),
    Idr0,
    (Idr0 & SMMU_IDR0_EXIDS) != 0,
    Idr1,
    mNumSmrg
    ));

  Valid = 0;
  for (Index = 0; Index < mNumSmrg; Index++) {
    if (!IsEntryValid (Index)) {
      continue;
    }

    Valid++;
    DEBUG ((
      DEBUG_INFO,
      "%a: EL2: entry %u SMR 0x%x S2CR 0x%x\n",
      __func__,
      Index,
      MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index)),
      MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))
      ));
  }

  DEBUG ((DEBUG_INFO, "%a: EL2: %u stream match entries in use\n", __func__, Valid));
}

/**
  Reads how many stream match groups and context banks the SMMU has, and
  where its register pages are. Under Gunyah these come from its emulation.

  @param[out]  Idr0  IDR0.
  @param[out]  Idr1  IDR1.
**/
STATIC
VOID
ReadGeometry (
  OUT UINT32  *Idr0,
  OUT UINT32  *Idr1
  )
{
  *Idr0 = MmioRead32 (APPS_SMMU_BASE + SMMU_IDR0);
  *Idr1 = MmioRead32 (APPS_SMMU_BASE + SMMU_IDR1);

  mNumSmrg     = *Idr0 & SMMU_IDR0_NUMSMRG;
  mPageShift   = ((*Idr1 & SMMU_IDR1_PAGESIZE) != 0) ? 16 : 12;
  mGr1Base     = APPS_SMMU_BASE + ((UINTN)1 << mPageShift);
  mCbBase      = APPS_SMMU_BASE + ((UINTN)1 << (SMMU_IDR1_NUMPAGENDXB (*Idr1) + 1 + mPageShift));
  mExtendedIds = (MmioRead32 (APPS_SMMU_BASE + SMMU_SCR0) & SMMU_SCR0_EXIDENABLE) != 0;
}

/**
  Implements QCS6490_SMMU_PROTOCOL.AfterGunyahExit().

  @param[in]  This  The protocol.

  @retval EFI_SUCCESS    Every recorded stream bypasses the SMMU.
  @retval EFI_NOT_READY  Not at EL2.
  @retval Other          A stream could not be set up.
**/
STATIC
EFI_STATUS
EFIAPI
AfterGunyahExit (
  IN QCS6490_SMMU_PROTOCOL  *This
  )
{
  UINT32      Idr0;
  UINT32      Idr1;
  UINTN       Index;
  EFI_STATUS  Status;
  EFI_STATUS  Result;

  if (ArmReadCurrentEL () != AARCH64_EL2) {
    return EFI_NOT_READY;
  }

  ReadGeometry (&Idr0, &Idr1);
  LogEl2State (Idr0, Idr1);

  Result = EFI_SUCCESS;
  for (Index = 0; Index < mPendingCount; Index++) {
    Status = HandOverNow (mPending[Index].Id, mPending[Index].Mask, mPending[Index].Name);
    if (EFI_ERROR (Status)) {
      Result = Status;
    }
  }

  mPendingCount = 0;
  return Result;
}

STATIC QCS6490_SMMU_PROTOCOL  mSmmuProtocol = {
  HandOverBypass,
  AfterGunyahExit
};

/**
  Tells the drivers that do DMA that their streams are set up.

  @retval EFI_SUCCESS  The marker protocol is installed.
  @retval Other        It could not be installed.
**/
STATIC
EFI_STATUS
InstallSmmuReady (
  VOID
  )
{
  EFI_HANDLE  Handle;

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gQcs6490SmmuReadyProtocolGuid,
                NULL,
                NULL
                );
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The streams bypass stage 1, or have nothing to do.
  @retval Other        The SMMU could not be set up.
**/
EFI_STATUS
EFIAPI
SmmuDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32      Idr0;
  UINT32      Idr1;
  UINTN       NumSmrg;
  UINTN       NumCb;
  UINTN       Stream;
  UINTN       Index;
  UINT32      Smr;
  UINT32      S2cr;
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  ReadGeometry (&Idr0, &Idr1);
  NumSmrg = mNumSmrg;
  NumCb   = SMMU_IDR1_NUMCB (Idr1);

  if (ArmReadCurrentEL () != AARCH64_EL1) {
    DEBUG ((DEBUG_INFO, "%a: not a Gunyah guest, UEFI's own streams left alone\n", __func__));
    LogEl2State (Idr0, Idr1);

    Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Handle,
                    &gQcs6490SmmuProtocolGuid,
                    &mSmmuProtocol,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    return InstallSmmuReady ();
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: IDR0 0x%x IDR1 0x%x, %u stream match groups, %u context banks%a\n",
    __func__,
    Idr0,
    Idr1,
    NumSmrg,
    NumCb,
    mExtendedIds ? ", extended IDs" : ""
    ));

  if (((Idr0 & SMMU_IDR0_SMS) == 0) || (NumSmrg == 0) || (NumCb == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: no stream matching, DMA will fault\n", __func__));
    return EFI_UNSUPPORTED;
  }

  for (Stream = 0; Stream < ARRAY_SIZE (mStreams); Stream++) {
    if (FindOverlappingEntry (NumSmrg, &mStreams[Stream], &Index)) {
      DEBUG ((
        DEBUG_WARN,
        "%a: %a stream 0x%x already matched by entry %u (SMR 0x%x, S2CR 0x%x), left alone\n",
        __func__,
        mStreams[Stream].Name,
        mStreams[Stream].Id,
        Index,
        MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index)),
        MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))
        ));
      continue;
    }

    for (Index = 0; Index < NumSmrg && IsEntryValid (Index); Index++) {
    }

    if (Index == NumSmrg) {
      DEBUG ((DEBUG_ERROR, "%a: no free stream match group for %a\n", __func__, mStreams[Stream].Name));
      continue;
    }

    mSavedEntries[mSavedEntryCount].Index = Index;
    mSavedEntries[mSavedEntryCount].Smr   = MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index));
    mSavedEntries[mSavedEntryCount].S2cr  = MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index));
    mSavedEntryCount++;

    //
    // Point the entry at its context first, as Linux does, then make it
    // match the stream.
    //
    S2cr = GetBypassS2cr (Index, NumCb);
    Smr  = mStreams[Stream].Id | ((UINT32)mStreams[Stream].Mask << SMMU_SMR_MASK_SHIFT);
    if (mExtendedIds) {
      MmioWrite32 (APPS_SMMU_BASE + SMMU_S2CR (Index), S2cr);
      MmioWrite32 (APPS_SMMU_BASE + SMMU_SMR (Index), Smr);
      MmioWrite32 (APPS_SMMU_BASE + SMMU_S2CR (Index), S2cr | SMMU_S2CR_EXIDVALID);
    } else {
      MmioWrite32 (APPS_SMMU_BASE + SMMU_S2CR (Index), S2cr);
      MmioWrite32 (APPS_SMMU_BASE + SMMU_SMR (Index), Smr | SMMU_SMR_VALID);
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: %a stream 0x%x: entry %u, SMR 0x%x, S2CR 0x%x\n",
      __func__,
      mStreams[Stream].Name,
      mStreams[Stream].Id,
      Index,
      MmioRead32 (APPS_SMMU_BASE + SMMU_SMR (Index)),
      MmioRead32 (APPS_SMMU_BASE + SMMU_S2CR (Index))
      ));
  }

  ArmDataSynchronizationBarrier ();

  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_NOTIFY,
                  RestoreSmmu,
                  NULL,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    RestoreSmmu (NULL, NULL);
    return Status;
  }

  //
  // The OS runs at EL2 once Gunyah has left at ExitBootServices: the remote
  // processors UEFI starts are handed over then.
  //
  if (Qcs6490GunyahExitDeferred ()) {
    DEBUG ((DEBUG_INFO, "%a: Gunyah leaves at ExitBootServices, streams handed over then\n", __func__));
    mDeferred = TRUE;
    Handle    = NULL;
    Status    = gBS->InstallMultipleProtocolInterfaces (
                       &Handle,
                       &gQcs6490SmmuProtocolGuid,
                       &mSmmuProtocol,
                       NULL
                       );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return InstallSmmuReady ();
}
