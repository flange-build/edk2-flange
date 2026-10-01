/** @file
  Early SEC initialization for QCS6490: loads the UEFI variable store from
  UFS into memory, and decides whether UEFI leaves Gunyah for EL2.

  ArmPlatformPeiBootAction() calls Qcs6490EarlyInit() first thing, at EL1 as
  a Gunyah guest, on a temporary stack at the top of the SEC primary stack,
  with exception vectors of its own (EarlyVectors.S), and with the MMU and
  caches off. No library constructor has run: only fixed PCDs, BaseLib,
  BaseMemoryLib, IoLib, ArmLib, CacheMaintenanceLib, PrintLib and the serial
  port XBL set up are used. Every access is to Device memory, so this code
  and those libraries (built with -mstrict-align) only make naturally
  aligned accesses.

  The steps:
    1. The status page (Qcs6490NvStore.h) is filled in as found.
    2. The apps SMMU lets UFS through, the way SmmuDxe does (EarlySmmu.c).
    3. The variable store partition is looked up in the GPT of its LUN and
       read into memory through the UFS controller XBL left running
       (EarlyUfs.c, EarlyNvStore.c); the SMMU is put back.
    4. The HypervisorMode and DspPreload variables are read from the
       store, and OsConfigTableSelection from xbl_config (XblConfig.c).
    5. The variable decides, then xbl_config, then PcdExitGunyah.
    6. For EL2, when the DSPs are to be preloaded (PcdGunyahLateExit), the
       exit waits for ExitBootServices: the DSPs only run when TrustZone
       starts them for a Gunyah guest. GunyahExitDxe leaves Gunyah then.

  Nothing here may stop the boot: every wait is bounded, and anything that
  fails leaves the variables in memory only, and the exception level to
  xbl_config and PcdExitGunyah. Each step prints one line.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi/UefiBaseType.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/PcdLib.h>

#include <Guid/Qcs6490PlatformConfig.h>
#include <Qcs6490NvStore.h>

#include "Qcs6490Early.h"
#include "Qcs6490LibInternal.h"

//
// The generic timer frequency XBL programs, for when CNTFRQ_EL0 reads
// nothing sensible.
//
#define QCS6490_TIMER_FREQUENCY  19200000

//
// The shortest time a poll can take (at least one register read). It bounds
// a wait by its number of polls in case the counter does not run.
//
#define QCS6490_MIN_POLL_NS  20

BOOLEAN  gQcs6490EarlyInitDone = FALSE;

STATIC BOOLEAN  mExceptionTaken = FALSE;

/**
  Starts a bounded wait.

  @param[out] Timeout       The wait.
  @param[in]  Microseconds  How long it may take.
**/
VOID
Qcs6490EarlyTimeoutStart (
  OUT QCS6490_EARLY_TIMEOUT  *Timeout,
  IN  UINT32                 Microseconds
  )
{
  UINT64  Frequency;

  Frequency = ArmReadCntFrq ();
  if ((Frequency == 0) || (Frequency > 1000000000)) {
    Frequency = QCS6490_TIMER_FREQUENCY;
  }

  Timeout->Start     = ArmReadCntvCt ();
  Timeout->Ticks     = DivU64x32 (MultU64x32 (Frequency, Microseconds), 1000000);
  Timeout->PollsLeft = MultU64x32 (Microseconds, 1000 / QCS6490_MIN_POLL_NS);
}

/**
  Checks a bounded wait, once per poll.

  @param[in, out] Timeout  The wait.

  @retval TRUE   The time is up.
  @retval FALSE  Keep polling.
**/
BOOLEAN
Qcs6490EarlyTimeoutExpired (
  IN OUT QCS6490_EARLY_TIMEOUT  *Timeout
  )
{
  if (Timeout->PollsLeft == 0) {
    return TRUE;
  }

  Timeout->PollsLeft--;
  return (ArmReadCntvCt () - Timeout->Start) >= Timeout->Ticks;
}

/**
  Returns whether two ranges overlap.

  @param[in]  BaseA  The start of the first range.
  @param[in]  SizeA  Its size.
  @param[in]  BaseB  The start of the second range.
  @param[in]  SizeB  Its size.

  @retval TRUE   They overlap.
  @retval FALSE  They do not.
**/
STATIC
BOOLEAN
RangesOverlap (
  IN UINT64  BaseA,
  IN UINT64  SizeA,
  IN UINT64  BaseB,
  IN UINT64  SizeB
  )
{
  return (BaseA < BaseB + SizeB) && (BaseB < BaseA + SizeA);
}

/**
  Checks that a range is DRAM that belongs to the OS and that UEFI does not
  use yet: inside the DRAM every QCS6490 board has, and clear of the
  firmware carve-outs, the firmware volume and the SEC stack.

  @param[in]  Base  The start of the range.
  @param[in]  Size  Its size in bytes.

  @retval TRUE   SEC may write the range and have devices write it.
  @retval FALSE  It may not.
**/
BOOLEAN
Qcs6490EarlyIsFreeDram (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  UINT64  DramBase;
  UINT64  DramEnd;
  UINTN   Index;

  //
  // The DRAM assumed when SMEM cannot be read is there on every board.
  //
  DramBase = FixedPcdGet64 (PcdSystemMemoryBase);
  DramEnd  = DramBase + FixedPcdGet64 (PcdFallbackSystemMemorySize);

  if ((Size == 0) || (Base < DramBase) || (Base >= DramEnd) || (Size > DramEnd - Base)) {
    return FALSE;
  }

  for (Index = 0; Index < gQcs6490CarveoutCount; Index++) {
    if (RangesOverlap (Base, Size, gQcs6490Carveouts[Index].Base, gQcs6490Carveouts[Index].Size)) {
      return FALSE;
    }
  }

  if (RangesOverlap (Base, Size, FixedPcdGet64 (PcdFdBaseAddress), FixedPcdGet32 (PcdFdSize))) {
    return FALSE;
  }

  if (RangesOverlap (
        Base,
        Size,
        FixedPcdGet64 (PcdCPUCoresStackBase),
        FixedPcdGet32 (PcdCPUCorePrimaryStackSize)
        ))
  {
    return FALSE;
  }

  return TRUE;
}

/**
  Checks that a range lies inside the store, status page and SEC scratch
  memory.

  @param[in]  Base  The start of the range.
  @param[in]  Size  Its size in bytes.

  @retval TRUE   It does.
  @retval FALSE  It does not, or is empty.
**/
BOOLEAN
Qcs6490EarlyIsInNvArea (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  return (Size != 0) &&
         (Base >= QCS6490_EARLY_NV_AREA_BASE) &&
         (Base - QCS6490_EARLY_NV_AREA_BASE < QCS6490_EARLY_NV_AREA_SIZE) &&
         (Size <= QCS6490_EARLY_NV_AREA_SIZE - (Base - QCS6490_EARLY_NV_AREA_BASE));
}

/**
  Checks the variable store layout the PCDs describe: contiguous regions of
  whole blocks, in DRAM nothing else uses.

  @retval TRUE   The store, status page and scratch memory may be used.
  @retval FALSE  They may not; nothing may be written there.
**/
BOOLEAN
Qcs6490NvStoreLayoutValid (
  VOID
  )
{
  UINT64  VariableSize;
  UINT64  WorkingSize;
  UINT64  SpareSize;

  VariableSize = FixedPcdGet32 (PcdFlashNvStorageVariableSize);
  WorkingSize  = FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize);
  SpareSize    = FixedPcdGet32 (PcdFlashNvStorageFtwSpareSize);

  if ((QCS6490_NVSTORE_BASE == 0) ||
      ((QCS6490_NVSTORE_BASE & (QCS6490_NVSTORE_BLOCK_SIZE - 1)) != 0) ||
      (VariableSize == 0) || (WorkingSize == 0) || (SpareSize == 0) ||
      (((VariableSize | WorkingSize | SpareSize) & (QCS6490_NVSTORE_BLOCK_SIZE - 1)) != 0))
  {
    return FALSE;
  }

  if ((FixedPcdGet64 (PcdFlashNvStorageFtwWorkingBase64) != QCS6490_NVSTORE_BASE + VariableSize) ||
      (FixedPcdGet64 (PcdFlashNvStorageFtwSpareBase64) != QCS6490_NVSTORE_BASE + VariableSize + WorkingSize))
  {
    return FALSE;
  }

  return Qcs6490EarlyIsFreeDram (QCS6490_EARLY_NV_AREA_BASE, QCS6490_EARLY_NV_AREA_SIZE);
}

/**
  Fills in the status page as SEC finds things before it reads the store.

  The layout must be valid (Qcs6490NvStoreLayoutValid ()).

  @param[in]  Decision  The exception level UEFI runs at so far,
                        QCS6490_HYPERVISOR_MODE_EL1 or _EL2.

  @return  The status page.
**/
QCS6490_NVSTORE_STATUS *
Qcs6490NvStoreStatusInit (
  IN UINT8  Decision
  )
{
  QCS6490_NVSTORE_STATUS  *Status;

  Status = (QCS6490_NVSTORE_STATUS *)(UINTN)QCS6490_NVSTORE_STATUS_BASE;
  ZeroMem (Status, QCS6490_NVSTORE_STATUS_SIZE);

  Status->Signature          = QCS6490_NVSTORE_STATUS_SIGNATURE;
  Status->Version            = QCS6490_NVSTORE_STATUS_VERSION;
  Status->Size               = sizeof (QCS6490_NVSTORE_STATUS);
  Status->LoadResult         = QCS6490_NVSTORE_LOAD_NOT_TRIED;
  Status->UfsDiagnostic      = 0;
  Status->PartitionLba       = 0;
  Status->PartitionBlocks    = 0;
  Status->BlockSize          = 0;
  Status->Lun                = FixedPcdGet8 (PcdNvStoreUfsLun);
  Status->HypervisorSetting  = QCS6490_HYPERVISOR_SETTING_NONE;
  Status->XblOsConfig        = QCS6490_XBL_OS_CONFIG_UNKNOWN;
  Status->HypervisorDecision = Decision;
  Status->ExitGunyahStatus   = QCS6490_SMC_NOT_ISSUED;
  Status->DspPreloadMode     = QCS6490_DSP_PRELOAD_SETTING_NONE;
  Status->DspPreloadDsps     = QCS6490_DSP_PRELOAD_ADSP | QCS6490_DSP_PRELOAD_CDSP;
  Status->GunyahExit         = (Decision == QCS6490_HYPERVISOR_MODE_EL2) ? QCS6490_GUNYAH_EXIT_SEC
                                                                          : QCS6490_GUNYAH_EXIT_NONE;
  Status->LateExitStatus     = QCS6490_SMC_NOT_ISSUED;

  return Status;
}

/**
  Returns the status page SEC filled in on this boot.

  @return  The status page, or NULL if the layout is not valid or the page
           was not filled in.
**/
QCS6490_NVSTORE_STATUS *
Qcs6490NvStoreStatusGet (
  VOID
  )
{
  QCS6490_NVSTORE_STATUS  *Status;

  if (!Qcs6490NvStoreLayoutValid ()) {
    return NULL;
  }

  Status = (QCS6490_NVSTORE_STATUS *)(UINTN)QCS6490_NVSTORE_STATUS_BASE;
  if ((Status->Signature != QCS6490_NVSTORE_STATUS_SIGNATURE) ||
      (Status->Version != QCS6490_NVSTORE_STATUS_VERSION) ||
      (Status->Size != sizeof (QCS6490_NVSTORE_STATUS)))
  {
    return NULL;
  }

  return Status;
}

/**
  Reads the store from its partition once UFS DMA is allowed, and the
  HypervisorMode variable from it.

  @param[in, out] Ufs     The controller.
  @param[in]      Lun     The LUN of the partition.
  @param[in]      Name    The name of the partition.
  @param[in, out] Status  The status page.
**/
STATIC
VOID
ReadNvStore (
  IN OUT QCS6490_EARLY_UFS       *Ufs,
  IN     UINT8                   Lun,
  IN     CONST CHAR16            *Name,
  IN OUT QCS6490_NVSTORE_STATUS  *Status
  )
{
  UINT64                      Lba;
  UINT64                      Blocks;
  RETURN_STATUS               Result;
  QCS6490_DSP_PRELOAD_CONFIG  DspPreload;

  Result = Qcs6490EarlyFindPartition (Ufs, Lun, Name, &Lba, &Blocks);
  if (RETURN_ERROR (Result)) {
    Status->UfsDiagnostic = Ufs->Diagnostic;
    if (QCS6490_UFS_DIAG_STEP (Ufs->Diagnostic) >= QCS6490_UFS_STEP_FIRST_GPT) {
      Status->LoadResult = QCS6490_NVSTORE_LOAD_NO_PARTITION;
      Qcs6490Print (
        "QCS6490: variables: no partition %s on LUN%u (0x%08x), kept in memory only\n",
        Name,
        Lun,
        Ufs->Diagnostic
        );
    } else {
      Status->LoadResult = QCS6490_NVSTORE_LOAD_UFS_ERROR;
      Qcs6490Print (
        "QCS6490: variables: GPT of LUN%u not read (0x%08x), kept in memory only\n",
        Lun,
        Ufs->Diagnostic
        );
    }

    return;
  }

  Status->PartitionLba    = Lba;
  Status->PartitionBlocks = Blocks;
  Status->BlockSize       = QCS6490_NVSTORE_BLOCK_SIZE;

  if (Blocks < QCS6490_NVSTORE_SIZE / QCS6490_NVSTORE_BLOCK_SIZE) {
    Status->LoadResult = QCS6490_NVSTORE_LOAD_TOO_SMALL;
    Qcs6490Print (
      "QCS6490: variables: LUN%u %s LBA %lu has %lu blocks, too small, kept in memory only\n",
      Lun,
      Name,
      Lba,
      Blocks
      );
    return;
  }

  Result = Qcs6490EarlyUfsRead (
             Ufs,
             Lun,
             Lba,
             QCS6490_NVSTORE_SIZE / QCS6490_NVSTORE_BLOCK_SIZE,
             QCS6490_NVSTORE_BASE
             );
  if (RETURN_ERROR (Result)) {
    Status->LoadResult    = QCS6490_NVSTORE_LOAD_UFS_ERROR;
    Status->UfsDiagnostic = Ufs->Diagnostic;
    Qcs6490Print (
      "QCS6490: variables: LUN%u %s LBA %lu not read (0x%08x), kept in memory only\n",
      Lun,
      Name,
      Lba,
      Ufs->Diagnostic
      );
    return;
  }

  if (!Qcs6490EarlyCheckStore ()) {
    Status->LoadResult = QCS6490_NVSTORE_LOAD_BLANK;
    Qcs6490Print (
      "QCS6490: variables: LUN%u %s LBA %lu, 0x%x bytes read, no variable store yet\n",
      Lun,
      Name,
      Lba,
      QCS6490_NVSTORE_SIZE
      );
    return;
  }

  Status->LoadResult        = QCS6490_NVSTORE_LOAD_OK;
  Status->HypervisorSetting = Qcs6490EarlyFindHypervisorMode ();
  if (Qcs6490EarlyFindDspPreload (&DspPreload)) {
    Status->DspPreloadMode = DspPreload.Mode;
    Status->DspPreloadDsps = (DspPreload.Adsp ? QCS6490_DSP_PRELOAD_ADSP : 0) |
                             (DspPreload.Cdsp ? QCS6490_DSP_PRELOAD_CDSP : 0);
  }

  Qcs6490Print (
    "QCS6490: variables: LUN%u %s LBA %lu, 0x%x bytes loaded\n",
    Lun,
    Name,
    Lba,
    QCS6490_NVSTORE_SIZE
    );
}

/**
  Reads the variable store from its UFS partition into memory.

  @param[in, out] Status  The status page, to fill in.
**/
STATIC
VOID
LoadNvStore (
  IN OUT QCS6490_NVSTORE_STATUS  *Status
  )
{
  CONST CHAR16        *Name;
  UINT8               Lun;
  QCS6490_EARLY_UFS   Ufs;
  QCS6490_EARLY_SMMU  Smmu;
  RETURN_STATUS       Result;

  Name = (CONST CHAR16 *)FixedPcdGetPtr (PcdNvStorePartitionName);
  Lun  = FixedPcdGet8 (PcdNvStoreUfsLun);

  if (Name[0] == L'\0') {
    Status->LoadResult = QCS6490_NVSTORE_LOAD_NO_PARTITION;
    Qcs6490Print ("QCS6490: variables: no partition configured, kept in memory only\n");
    return;
  }

  //
  // Before the first UFS register access: if the boot stops right after this
  // line, the controller did not answer.
  //
  Qcs6490Print ("QCS6490: variables: reading LUN%u %s from UFS\n", Lun, Name);

  Result = Qcs6490EarlyUfsInit (&Ufs);
  if (RETURN_ERROR (Result)) {
    Qcs6490EarlyUfsFinish (&Ufs);
    Status->LoadResult    = QCS6490_NVSTORE_LOAD_UFS_ERROR;
    Status->UfsDiagnostic = Ufs.Diagnostic;
    Qcs6490Print (
      "QCS6490: variables: UFS not usable (0x%08x), kept in memory only\n",
      Ufs.Diagnostic
      );
    return;
  }

  Result = Qcs6490EarlySmmuEnableUfs (&Smmu);
  if (RETURN_ERROR (Result)) {
    Qcs6490EarlyUfsFinish (&Ufs);
    Status->LoadResult = QCS6490_NVSTORE_LOAD_SMMU_ERROR;
    Qcs6490Print ("QCS6490: variables: no SMMU entry for UFS, kept in memory only\n");
    return;
  }

  ReadNvStore (&Ufs, Lun, Name, Status);

  //
  // Put the SMMU back only once UFS is done with DMA: a transfer the SMMU
  // faults takes the system down.
  //
  if (Qcs6490EarlyUfsFinish (&Ufs)) {
    Qcs6490EarlySmmuRestore (&Smmu);
  } else {
    Qcs6490Print ("QCS6490: UFS: a request is still pending, SMMU entry left in place\n");
  }

  //
  // Drop whatever the caches may have picked up of what UFS wrote.
  //
  InvalidateDataCacheRange ((VOID *)(UINTN)QCS6490_EARLY_NV_AREA_BASE, QCS6490_EARLY_NV_AREA_SIZE);
}

/**
  Names a HypervisorMode value for the console.

  @param[in]  Setting  The value, or QCS6490_HYPERVISOR_SETTING_NONE.

  @return  Its name.
**/
STATIC
CONST CHAR8 *
SettingName (
  IN UINT8  Setting
  )
{
  switch (Setting) {
    case QCS6490_HYPERVISOR_MODE_AUTO:
      return "Auto";
    case QCS6490_HYPERVISOR_MODE_EL1:
      return "EL1";
    case QCS6490_HYPERVISOR_MODE_EL2:
      return "EL2";
    default:
      return "not set";
  }
}

/**
  Names an OsConfigTableSelection value for the console.

  @param[in]  XblOsConfig  The value, or QCS6490_XBL_OS_CONFIG_UNKNOWN.

  @return  Its name.
**/
STATIC
CONST CHAR8 *
XblOsConfigName (
  IN UINT8  XblOsConfig
  )
{
  switch (XblOsConfig) {
    case QCS6490_XBL_OS_CONFIG_GUNYAH:
      return "Gunyah";
    case QCS6490_XBL_OS_CONFIG_KVM:
      return "KVM";
    default:
      return "unknown";
  }
}

/**
  Decides the exception level: the HypervisorMode variable when it says EL1
  or EL2, else xbl_config the way the stock Qualcomm UEFI follows it, else
  PcdExitGunyah.

  @param[in]  Setting      HypervisorMode, or QCS6490_HYPERVISOR_SETTING_NONE.
  @param[in]  XblOsConfig  OsConfigTableSelection, or
                           QCS6490_XBL_OS_CONFIG_UNKNOWN.

  @return  QCS6490_HYPERVISOR_MODE_EL1 or QCS6490_HYPERVISOR_MODE_EL2.
**/
STATIC
UINT8
DecideHypervisor (
  IN UINT8  Setting,
  IN UINT8  XblOsConfig
  )
{
  UINT8        Decision;
  CONST CHAR8  *Default;

  Default = "";
  if ((Setting == QCS6490_HYPERVISOR_MODE_EL1) || (Setting == QCS6490_HYPERVISOR_MODE_EL2)) {
    Decision = Setting;
  } else if (XblOsConfig == QCS6490_XBL_OS_CONFIG_GUNYAH) {
    Decision = QCS6490_HYPERVISOR_MODE_EL1;
  } else if (XblOsConfig == QCS6490_XBL_OS_CONFIG_KVM) {
    Decision = QCS6490_HYPERVISOR_MODE_EL2;
  } else {
    Decision = FixedPcdGetBool (PcdExitGunyah) ? QCS6490_HYPERVISOR_MODE_EL2
                                               : QCS6490_HYPERVISOR_MODE_EL1;
    Default = ", build default";
  }

  //
  // For EL2, DeferExit () says next when Gunyah leaves.
  //
  Qcs6490Print (
    "QCS6490: hypervisor: setting %a, xbl_config %a -> EL%u (%a%a)\n",
    SettingName (Setting),
    XblOsConfigName (XblOsConfig),
    (Decision == QCS6490_HYPERVISOR_MODE_EL2) ? 2 : 1,
    (Decision == QCS6490_HYPERVISOR_MODE_EL2) ? "KVM" : "keeping Gunyah",
    Default
    );

  return Decision;
}

/**
  For a boot that runs the OS at EL2, decides whether to leave Gunyah now or
  only once the OS loader's ExitBootServices succeeded (GunyahExitDxe), as
  PcdGunyahLateExit says: TrustZone starts the DSPs for a Gunyah guest
  only, so they can only be preloaded before Gunyah leaves.

  @param[in]  Status  The status page, or NULL.

  @retval TRUE   Leave Gunyah at ExitBootServices.
  @retval FALSE  Leave it now.
**/
STATIC
BOOLEAN
DeferExit (
  IN CONST QCS6490_NVSTORE_STATUS  *Status
  )
{
  BOOLEAN      Preload;
  CONST CHAR8  *Why;

  //
  // DXE learns of the deferral from the status page only.
  //
  if (Status == NULL) {
    Qcs6490Print ("QCS6490: hypervisor: leaving Gunyah now (no status page for DXE)\n");
    return FALSE;
  }

  Preload = (Status->DspPreloadMode != QCS6490_DSP_PRELOAD_DISABLED) && (Status->DspPreloadDsps != 0);

  switch (FixedPcdGet8 (PcdGunyahLateExit)) {
    case QCS6490_GUNYAH_LATE_EXIT_NEVER:
      Qcs6490Print ("QCS6490: hypervisor: leaving Gunyah now (build setting)\n");
      return FALSE;

    case QCS6490_GUNYAH_LATE_EXIT_ALWAYS:
      Why = "build setting";
      break;

    default:
      if (!Preload) {
        Qcs6490Print ("QCS6490: hypervisor: leaving Gunyah now (DSP preload off)\n");
        return FALSE;
      }

      Why = "DSP preload";
      break;
  }

  Qcs6490Print (
    "QCS6490: hypervisor: Gunyah stays until ExitBootServices (%a%a)\n",
    Why,
    (Status->DspPreloadMode == QCS6490_DSP_PRELOAD_SETTING_NONE) ? ", default setting" : ""
    );
  return TRUE;
}

/**
  Early SEC initialization: loads the variable store from UFS and decides
  whether to leave Gunyah. Called by ArmPlatformPeiBootAction() at EL1.

  @retval TRUE   Ask TrustZone to remove Gunyah and continue at EL2.
  @retval FALSE  Stay a Gunyah guest at EL1.
**/
BOOLEAN
EFIAPI
Qcs6490EarlyInit (
  VOID
  )
{
  QCS6490_NVSTORE_STATUS  *Status;
  UINT8                   Setting;
  UINT8                   XblOsConfig;
  UINT8                   Decision;

  gQcs6490EarlyInitDone = TRUE;

  Status  = NULL;
  Setting = QCS6490_HYPERVISOR_SETTING_NONE;

  if (Qcs6490NvStoreLayoutValid ()) {
    //
    // Write back and drop whatever the caches may hold of the store before
    // SEC writes it with the caches off and UFS writes it by DMA.
    //
    WriteBackInvalidateDataCacheRange (
      (VOID *)(UINTN)QCS6490_EARLY_NV_AREA_BASE,
      QCS6490_EARLY_NV_AREA_SIZE
      );

    Status = Qcs6490NvStoreStatusInit (QCS6490_HYPERVISOR_MODE_EL1);
    LoadNvStore (Status);
    Setting = Status->HypervisorSetting;
  } else {
    Qcs6490Print ("QCS6490: variables: no valid store layout in the PCDs, kept in memory only\n");
  }

  XblOsConfig = Qcs6490EarlyReadXblConfig ();
  Decision    = DecideHypervisor (Setting, XblOsConfig);

  if (Status != NULL) {
    Status->XblOsConfig        = XblOsConfig;
    Status->HypervisorDecision = Decision;
  }

  gQcs6490DeferExitGunyah = (Decision == QCS6490_HYPERVISOR_MODE_EL2) && DeferExit (Status);
  gQcs6490ExitGunyah      = (Decision == QCS6490_HYPERVISOR_MODE_EL2) && !gQcs6490DeferExitGunyah;

  if (Status != NULL) {
    Status->GunyahExit = gQcs6490DeferExitGunyah ? QCS6490_GUNYAH_EXIT_EXIT_BOOT_SERVICES
                         : (gQcs6490ExitGunyah ? QCS6490_GUNYAH_EXIT_SEC : QCS6490_GUNYAH_EXIT_NONE);
  }

  return gQcs6490ExitGunyah;
}

/**
  Reports an exception taken while the early vectors are installed. The
  caller parks the CPU afterwards.

  @param[in]  Vector  The offset of the vector taken.
  @param[in]  Esr     ESR_EL1.
  @param[in]  Elr     ELR_EL1.
  @param[in]  Far     FAR_EL1.
  @param[in]  Spsr    SPSR_EL1.
**/
VOID
EFIAPI
Qcs6490EarlyException (
  IN UINTN  Vector,
  IN UINTN  Esr,
  IN UINTN  Elr,
  IN UINTN  Far,
  IN UINTN  Spsr
  )
{
  CONST CHAR8  *Kind;

  //
  // A fault while reporting one: just park.
  //
  if (mExceptionTaken) {
    return;
  }

  mExceptionTaken = TRUE;

  switch (Vector & 0x180) {
    case 0x000:
      Kind = "synchronous exception";
      break;
    case 0x080:
      Kind = "IRQ";
      break;
    case 0x100:
      Kind = "FIQ";
      break;
    default:
      Kind = "SError";
      break;
  }

  Qcs6490Print ("\nQCS6490: %a in early SEC (vector 0x%03lx)\n", Kind, Vector);
  Qcs6490Print (
    "QCS6490: ESR 0x%08lx ELR 0x%lx FAR 0x%lx SPSR 0x%lx\n",
    Esr,
    Elr,
    Far,
    Spsr
    );
  Qcs6490Print ("QCS6490: stopped\n");
}
