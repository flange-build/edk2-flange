/** @file
  The QCS6490 non-volatile variable store: the UEFI variable, FTW working
  and FTW spare regions, which live on UFS and are read into memory by SEC.

  Memory layout, from PcdFlashNvStorageVariableBase64 up, one copy of the
  first bytes of the UFS partition named by PcdNvStorePartitionName on LUN
  PcdNvStoreUfsLun:

    Variable store   PcdFlashNvStorageVariableBase64,   PcdFlashNvStorageVariableSize
    FTW working      PcdFlashNvStorageFtwWorkingBase64, PcdFlashNvStorageFtwWorkingSize
    FTW spare        PcdFlashNvStorageFtwSpareBase64,   PcdFlashNvStorageFtwSpareSize
    Status page      QCS6490_NVSTORE_STATUS_BASE, one 4 KiB page (not on UFS)

  The three regions are contiguous in that order, and in the same order at
  the start of the partition. PEI reserves the whole range, status page
  included, as EfiRuntimeServicesData.

  SEC also uses the page after the status page as scratch memory for its UFS
  transfers, before PEI runs; nothing is kept there.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_NV_STORE_H_
#define QCS6490_NV_STORE_H_

#define QCS6490_NVSTORE_BASE  FixedPcdGet64 (PcdFlashNvStorageVariableBase64)

//
// Bytes of the store that live on UFS: variable + FTW working + FTW spare.
//
#define QCS6490_NVSTORE_SIZE                            \
  (FixedPcdGet32 (PcdFlashNvStorageVariableSize) +      \
   FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize) +    \
   FixedPcdGet32 (PcdFlashNvStorageFtwSpareSize))

#define QCS6490_NVSTORE_STATUS_BASE  (QCS6490_NVSTORE_BASE + QCS6490_NVSTORE_SIZE)
#define QCS6490_NVSTORE_STATUS_SIZE  SIZE_4KB

//
// What PEI reserves: the store and the status page.
//
#define QCS6490_NVSTORE_RESERVED_SIZE  (QCS6490_NVSTORE_SIZE + QCS6490_NVSTORE_STATUS_SIZE)

//
// SEC scratch for UFS transfer descriptors, right after the status page.
//
#define QCS6490_NVSTORE_SCRATCH_BASE  (QCS6490_NVSTORE_STATUS_BASE + QCS6490_NVSTORE_STATUS_SIZE)
#define QCS6490_NVSTORE_SCRATCH_SIZE  SIZE_64KB

//
// UFS logical block size the store is kept in; also the FVB block size.
//
#define QCS6490_NVSTORE_BLOCK_SIZE  SIZE_4KB

#define QCS6490_NVSTORE_STATUS_SIGNATURE  SIGNATURE_32 ('Q', 'N', 'V', 'S')
#define QCS6490_NVSTORE_STATUS_VERSION    2

//
// QCS6490_NVSTORE_STATUS.LoadResult
//
#define QCS6490_NVSTORE_LOAD_NOT_TRIED     0   // SEC did not get to it
#define QCS6490_NVSTORE_LOAD_OK            1   // read, and it holds a valid variable FV
#define QCS6490_NVSTORE_LOAD_BLANK         2   // read, but no valid variable FV (new partition)
#define QCS6490_NVSTORE_LOAD_NO_PARTITION  3   // no such partition on the LUN
#define QCS6490_NVSTORE_LOAD_UFS_ERROR     4   // UFS transfer failed; UfsDiagnostic says why
#define QCS6490_NVSTORE_LOAD_SMMU_ERROR    5   // no SMMU stream entry for UFS DMA
#define QCS6490_NVSTORE_LOAD_TOO_SMALL     6   // the partition is smaller than the store

//
// QCS6490_NVSTORE_STATUS.HypervisorSetting when the variable is missing or
// invalid.
//
#define QCS6490_HYPERVISOR_SETTING_NONE  0xFF

//
// QCS6490_NVSTORE_STATUS.DspPreloadMode when the DspPreload variable is
// missing or invalid; SEC then goes by its default (Auto, both DSPs).
//
#define QCS6490_DSP_PRELOAD_SETTING_NONE  0xFF

//
// QCS6490_NVSTORE_STATUS.DspPreloadDsps
//
#define QCS6490_DSP_PRELOAD_ADSP  BIT0
#define QCS6490_DSP_PRELOAD_CDSP  BIT1

//
// QCS6490_NVSTORE_STATUS.GunyahExit: when UEFI leaves Gunyah.
//
#define QCS6490_GUNYAH_EXIT_NONE                0   // never: UEFI and the OS stay at EL1
#define QCS6490_GUNYAH_EXIT_SEC                 1   // first thing in SEC
#define QCS6490_GUNYAH_EXIT_EXIT_BOOT_SERVICES  2   // once the OS loader's ExitBootServices succeeded

//
// PcdGunyahLateExit: when a boot that runs the OS at EL2 leaves Gunyah.
//
#define QCS6490_GUNYAH_LATE_EXIT_NEVER    0   // always in SEC
#define QCS6490_GUNYAH_LATE_EXIT_PRELOAD  1   // at ExitBootServices when the DSPs are preloaded
#define QCS6490_GUNYAH_LATE_EXIT_ALWAYS   2   // always at ExitBootServices

//
// QCS6490_NVSTORE_STATUS.ExitGunyahStatus and .LateExitStatus when the call
// was not made.
//
#define QCS6490_NVSTORE_SMC_NOT_ISSUED  MAX_INT32

//
// QCS6490_NVSTORE_STATUS.XblOsConfig when xbl_config has no
// OsConfigTableSelection (0) or it cannot be read.
//
#define QCS6490_XBL_OS_CONFIG_UNKNOWN  0

//
// Filled in by SEC, read by the FVB driver (whether memory holds what is on
// UFS) and by the setup UI (what was decided and why).
//
typedef struct {
  UINT32    Signature;            // QCS6490_NVSTORE_STATUS_SIGNATURE
  UINT16    Version;              // QCS6490_NVSTORE_STATUS_VERSION
  UINT16    Size;                 // sizeof (QCS6490_NVSTORE_STATUS)
  UINT32    LoadResult;           // QCS6490_NVSTORE_LOAD_*
  UINT32    UfsDiagnostic;        // free-form detail for the log (OCS, SCSI status, step)
  UINT64    PartitionLba;         // first block of the partition, 0 if not found
  UINT64    PartitionBlocks;      // its size in blocks
  UINT32    BlockSize;            // bytes per block of the LUN
  UINT8     Lun;                  // the LUN searched
  UINT8     HypervisorSetting;    // HypervisorMode as found, QCS6490_HYPERVISOR_SETTING_NONE if absent
  UINT8     XblOsConfig;          // xbl_config OsConfigTableSelection, QCS6490_XBL_OS_CONFIG_UNKNOWN if not found
  UINT8     HypervisorDecision;   // QCS6490_HYPERVISOR_MODE_EL1 or _EL2: what SEC asked for
  INT32     ExitGunyahStatus;     // TZ result of the exit call, MAX_INT32 if not issued
  UINT32    Reserved;
  UINT8     DspPreloadMode;       // DspPreload Mode as found, QCS6490_DSP_PRELOAD_SETTING_NONE if absent
  UINT8     DspPreloadDsps;       // QCS6490_DSP_PRELOAD_ADSP and _CDSP, as SEC went by
  UINT8     GunyahExit;           // QCS6490_GUNYAH_EXIT_*: when UEFI leaves (or left) Gunyah
  UINT8     Reserved2;
  INT32     LateExitStatus;       // Gunyah's result of the exit call at ExitBootServices, MAX_INT32 if not issued
} QCS6490_NVSTORE_STATUS;

#endif // QCS6490_NV_STORE_H_
