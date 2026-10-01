/** @file
  Adds the Platform Configuration formset of the QCS6490 platforms to the
  Device Manager: the Hypervisor setting, which picks the exception level
  UEFI and the OS run at, and what the boot firmware and SEC made of it at
  this boot; and the DSP preload setting (DspPreloadDxe).

  The setting is the HypervisorMode variable (Guid/Qcs6490PlatformConfig.h).
  SEC reads it straight from the variable store on UFS before it decides
  whether to leave Gunyah, so a change applies at the next boot, and only
  when the store is on UFS. What SEC found and decided is in the status page
  it leaves after the store (Qcs6490NvStore.h), which the formset shows next
  to the exception level UEFI runs at.

  The formset keeps its setting in an EFI variable store only, which the
  setup browser and the config routing protocol read and write themselves:
  no HII Config Access protocol is needed. Config routing writes a question
  back by reading the variable, changing the question's bytes and setting the
  variable again, so the variable has to exist; it is created here.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/DevicePath.h>

#include <Qcs6490NvStore.h>

#include "PlatformConfigDxe.h"

//
// The settings are non-volatile and boot services only, as the formset's
// EFI variable stores declare them.
//
#define SETTING_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

STATIC CONST QCS6490_HYPERVISOR_CONFIG  mDefaultHypervisorConfig = {
  QCS6490_HYPERVISOR_MODE_AUTO
};

STATIC CONST QCS6490_DSP_PRELOAD_CONFIG  mDefaultDspPreloadConfig = {
  QCS6490_DSP_PRELOAD_AUTO,
  TRUE,
  TRUE
};

//
// xbl_config's /sw/uefi/uefiplat OsConfigTableSelection.
//
#define XBL_OS_CONFIG_GUNYAH  1
#define XBL_OS_CONFIG_KVM     2

//
// QCS6490_NVSTORE_STATUS.ExitGunyahStatus when TrustZone was not called.
//
#define EXIT_GUNYAH_NOT_ISSUED  MAX_INT32

//
// Longest status value, in characters, terminator included.
//
#define STATUS_VALUE_LENGTH  128

//
// Longest GPT partition name, in characters.
//
#define GPT_PARTITION_NAME_LENGTH  36

//
// The memory map may grow while it is read; read it this many times at most.
//
#define MEMORY_MAP_ATTEMPTS  4

typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;

STATIC HII_VENDOR_DEVICE_PATH  mVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    QCS6490_PLATFORM_CONFIG_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL)),
      (UINT8)((sizeof (EFI_DEVICE_PATH_PROTOCOL)) >> 8)
    }
  }
};

/**
  Tells whether a range lies in memory the UEFI memory map has as
  EfiRuntimeServicesData, as PEI reserves the variable store and its status
  page. Anything else, an unmapped carve-out for instance, is not to be read.

  @param[in]  Base    The start of the range.
  @param[in]  Length  Its size in bytes.

  @retval TRUE   One EfiRuntimeServicesData descriptor covers the range.
  @retval FALSE  None does, or the memory map could not be read.
**/
STATIC
BOOLEAN
IsRuntimeServicesData (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Length
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Entry;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Attempt;
  UINTN                  Offset;
  UINT64                 EntrySize;
  BOOLEAN                Result;

  Map            = NULL;
  MapSize        = 0;
  DescriptorSize = sizeof (EFI_MEMORY_DESCRIPTOR);
  Status         = EFI_BUFFER_TOO_SMALL;

  for (Attempt = 0; Attempt < MEMORY_MAP_ATTEMPTS; Attempt++) {
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      break;
    }

    if (Map != NULL) {
      FreePool (Map);
    }

    //
    // Leave room for the descriptors the allocation itself may add.
    //
    MapSize += 4 * DescriptorSize;
    Map      = AllocatePool (MapSize);
    if (Map == NULL) {
      return FALSE;
    }
  }

  if (EFI_ERROR (Status) || (Map == NULL) || (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    DEBUG ((DEBUG_ERROR, "%a: cannot read the memory map: %r\n", __func__, Status));
    if (Map != NULL) {
      FreePool (Map);
    }

    return FALSE;
  }

  Result = FALSE;
  for (Offset = 0; Offset + sizeof (EFI_MEMORY_DESCRIPTOR) <= MapSize; Offset += DescriptorSize) {
    Entry     = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    EntrySize = EFI_PAGES_TO_SIZE (Entry->NumberOfPages);
    if ((Base >= Entry->PhysicalStart) && (Base - Entry->PhysicalStart < EntrySize)) {
      Result = (Entry->Type == EfiRuntimeServicesData) &&
               (Length <= EntrySize - (Base - Entry->PhysicalStart));
      break;
    }
  }

  FreePool (Map);
  return Result;
}

/**
  Reads the status page SEC leaves after the variable store.

  @param[out]  NvStatus  A copy of the status page.

  @retval TRUE   NvStatus holds what SEC wrote.
  @retval FALSE  There is no valid status page; NvStatus is undefined.
**/
STATIC
BOOLEAN
GetNvStoreStatus (
  OUT QCS6490_NVSTORE_STATUS  *NvStatus
  )
{
  if (!IsRuntimeServicesData (QCS6490_NVSTORE_STATUS_BASE, sizeof (*NvStatus))) {
    DEBUG ((
      DEBUG_WARN,
      "%a: 0x%lx is not reserved runtime data, no status from SEC\n",
      __func__,
      (UINT64)QCS6490_NVSTORE_STATUS_BASE
      ));
    return FALSE;
  }

  CopyMem (NvStatus, (VOID *)(UINTN)QCS6490_NVSTORE_STATUS_BASE, sizeof (*NvStatus));

  //
  // The same checks as the FVB driver's, so that both take the page or
  // neither does.
  //
  if ((NvStatus->Signature != QCS6490_NVSTORE_STATUS_SIGNATURE) ||
      (NvStatus->Version != QCS6490_NVSTORE_STATUS_VERSION) ||
      (NvStatus->Size != sizeof (*NvStatus)))
  {
    DEBUG ((
      DEBUG_WARN,
      "%a: no status from SEC (signature 0x%x, version %u, size %u)\n",
      __func__,
      NvStatus->Signature,
      NvStatus->Version,
      NvStatus->Size
      ));
    return FALSE;
  }

  return TRUE;
}

/**
  Returns the name of the partition the variable store lives in,
  PcdNvStorePartitionName, bounded to the length of a GPT partition name.

  @param[out]  Name        The name, empty if none is configured.
  @param[in]   NameLength  The size of Name in characters.
**/
STATIC
VOID
GetPartitionName (
  OUT CHAR16  *Name,
  IN  UINTN   NameLength
  )
{
  CONST CHAR16  *PcdName;
  UINTN         Length;

  PcdName = (CONST CHAR16 *)FixedPcdGetPtr (PcdNvStorePartitionName);
  Length  = StrnLenS (PcdName, FixedPcdGetSize (PcdNvStorePartitionName) / sizeof (CHAR16));
  Length  = MIN (Length, NameLength - 1);

  CopyMem (Name, PcdName, Length * sizeof (CHAR16));
  Name[Length] = L'\0';
}

/**
  Tells whether the partition SEC read is the one the store is written back
  to: the checks the FVB driver makes before it trusts a status page that
  says the store was loaded (or found blank), and without which it keeps the
  variables in memory only.

  @param[in]  NvStatus  The status page.

  @retval TRUE   The LUN, block size and partition size match the store.
  @retval FALSE  They do not.
**/
STATIC
BOOLEAN
StatusMatchesStore (
  IN CONST QCS6490_NVSTORE_STATUS  *NvStatus
  )
{
  return (NvStatus->Lun == FixedPcdGet8 (PcdNvStoreUfsLun)) &&
         (NvStatus->BlockSize == QCS6490_NVSTORE_BLOCK_SIZE) &&
         (NvStatus->PartitionLba != 0) &&
         (NvStatus->PartitionBlocks >= QCS6490_NVSTORE_SIZE / QCS6490_NVSTORE_BLOCK_SIZE);
}

/**
  Formats one of the strings of the formset, in one language.

  @param[in]   HiiHandle   The formset's HII handle.
  @param[in]   Language    The language to take the format string in.
  @param[in]   Format      The format string, one of the STR_* strings.
  @param[out]  Buffer      The formatted string; empty if the format string
                           could not be read.
  @param[in]   BufferSize  The size of Buffer in bytes.
  @param[in]   ...         The arguments of the format string.
**/
STATIC
VOID
FormatHiiString (
  IN  EFI_HII_HANDLE  HiiHandle,
  IN  CONST CHAR8     *Language,
  IN  EFI_STRING_ID   Format,
  OUT CHAR16          *Buffer,
  IN  UINTN           BufferSize,
  ...
  )
{
  EFI_STRING  Template;
  VA_LIST     Marker;

  Buffer[0] = L'\0';

  Template = HiiGetString (HiiHandle, Format, Language);
  if (Template == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: no string 0x%x in %a\n", __func__, Format, Language));
    return;
  }

  VA_START (Marker, BufferSize);
  UnicodeVSPrint (Buffer, BufferSize, Template, Marker);
  VA_END (Marker);

  FreePool (Template);
}

/**
  Sets the value of one of the status lines in one language. An empty value
  leaves the line at the "Unknown" of the string package.

  @param[in]  HiiHandle  The formset's HII handle.
  @param[in]  Language   The language to set the value in.
  @param[in]  Token      The value string of the line, STR_*_VALUE.
  @param[in]  Value      The value.
**/
STATIC
VOID
SetStatusValue (
  IN EFI_HII_HANDLE  HiiHandle,
  IN CONST CHAR8     *Language,
  IN EFI_STRING_ID   Token,
  IN CHAR16          *Value
  )
{
  if (Value[0] == L'\0') {
    return;
  }

  if (HiiSetString (HiiHandle, Token, Value, Language) == 0) {
    DEBUG ((DEBUG_ERROR, "%a: cannot set string 0x%x in %a\n", __func__, Token, Language));
  }
}

/**
  Fills in the current exception level.

  @param[in]  HiiHandle  The formset's HII handle.
  @param[in]  Language   The language to fill it in in.
  @param[in]  NvStatus   The status page, NULL if there is none.
**/
STATIC
VOID
UpdateExceptionLevel (
  IN EFI_HII_HANDLE                HiiHandle,
  IN CONST CHAR8                   *Language,
  IN CONST QCS6490_NVSTORE_STATUS  *NvStatus  OPTIONAL
  )
{
  CHAR16  Value[STATUS_VALUE_LENGTH];
  UINTN   CurrentEl;

  CurrentEl = ArmReadCurrentEL ();
  if ((CurrentEl == AARCH64_EL1) && (NvStatus != NULL) &&
      (NvStatus->GunyahExit == QCS6490_GUNYAH_EXIT_EXIT_BOOT_SERVICES))
  {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL1_UNTIL_BOOT), Value, sizeof (Value));
  } else if (CurrentEl == AARCH64_EL1) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL1), Value, sizeof (Value));
  } else if (CurrentEl == AARCH64_EL2) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL2), Value, sizeof (Value));
  } else {
    FormatHiiString (
      HiiHandle,
      Language,
      STRING_TOKEN (STR_VALUE_EL_OTHER),
      Value,
      sizeof (Value),
      (UINT32)(CurrentEl >> 2)
      );
  }

  SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_EL_VALUE), Value);
}

/**
  Fills in what xbl_config asks for.

  @param[in]  HiiHandle  The formset's HII handle.
  @param[in]  Language   The language to fill it in in.
  @param[in]  NvStatus   The status page, NULL if there is none.
**/
STATIC
VOID
UpdateXblConfig (
  IN EFI_HII_HANDLE                HiiHandle,
  IN CONST CHAR8                   *Language,
  IN CONST QCS6490_NVSTORE_STATUS  *NvStatus  OPTIONAL
  )
{
  CHAR16  Value[STATUS_VALUE_LENGTH];

  if (NvStatus == NULL) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_VALUE_NO_STATUS), Value, sizeof (Value));
  } else if (NvStatus->XblOsConfig == XBL_OS_CONFIG_GUNYAH) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL1), Value, sizeof (Value));
  } else if (NvStatus->XblOsConfig == XBL_OS_CONFIG_KVM) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL2), Value, sizeof (Value));
  } else if (NvStatus->XblOsConfig == QCS6490_XBL_OS_CONFIG_UNKNOWN) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_VALUE_NOT_FOUND), Value, sizeof (Value));
  } else {
    FormatHiiString (
      HiiHandle,
      Language,
      STRING_TOKEN (STR_VALUE_UNSUPPORTED),
      Value,
      sizeof (Value),
      (UINT32)NvStatus->XblOsConfig
      );
  }

  SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_XBL_VALUE), Value);
}

/**
  Tells what made SEC's decision, following the order SEC decides in: the
  Hypervisor setting when it is EL1 or EL2, else xbl_config when it says
  Gunyah or KVM, else PcdExitGunyah.

  @param[in]  NvStatus  The status page.

  @return  The format string of the decision, STR_DECISION_*.
**/
STATIC
EFI_STRING_ID
GetDecisionFormat (
  IN CONST QCS6490_NVSTORE_STATUS  *NvStatus
  )
{
  if ((NvStatus->HypervisorSetting == QCS6490_HYPERVISOR_MODE_EL1) ||
      (NvStatus->HypervisorSetting == QCS6490_HYPERVISOR_MODE_EL2))
  {
    return STRING_TOKEN (STR_DECISION_SETTING);
  }

  if ((NvStatus->XblOsConfig == XBL_OS_CONFIG_GUNYAH) ||
      (NvStatus->XblOsConfig == XBL_OS_CONFIG_KVM))
  {
    return STRING_TOKEN (STR_DECISION_XBL);
  }

  return STRING_TOKEN (STR_DECISION_DEFAULT);
}

/**
  Fills in what SEC decided and why, and what TrustZone answered when it
  did not go through.

  @param[in]  HiiHandle  The formset's HII handle.
  @param[in]  Language   The language to fill it in in.
  @param[in]  NvStatus   The status page, NULL if there is none.
**/
STATIC
VOID
UpdateDecision (
  IN EFI_HII_HANDLE                HiiHandle,
  IN CONST CHAR8                   *Language,
  IN CONST QCS6490_NVSTORE_STATUS  *NvStatus  OPTIONAL
  )
{
  CHAR16  Level[STATUS_VALUE_LENGTH];
  CHAR16  Decision[STATUS_VALUE_LENGTH];
  CHAR16  Value[STATUS_VALUE_LENGTH];

  if (NvStatus == NULL) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_VALUE_NO_STATUS), Value, sizeof (Value));
    SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_DECISION_VALUE), Value);
    return;
  }

  if (NvStatus->HypervisorDecision == QCS6490_HYPERVISOR_MODE_EL1) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL1), Level, sizeof (Level));
  } else if (NvStatus->HypervisorDecision == QCS6490_HYPERVISOR_MODE_EL2) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_HYPERVISOR_EL2), Level, sizeof (Level));
  } else {
    FormatHiiString (
      HiiHandle,
      Language,
      STRING_TOKEN (STR_VALUE_UNSUPPORTED),
      Level,
      sizeof (Level),
      (UINT32)NvStatus->HypervisorDecision
      );
  }

  FormatHiiString (HiiHandle, Language, GetDecisionFormat (NvStatus), Decision, sizeof (Decision), Level);

  if ((NvStatus->ExitGunyahStatus == 0) || (NvStatus->ExitGunyahStatus == EXIT_GUNYAH_NOT_ISSUED)) {
    SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_DECISION_VALUE), Decision);
    return;
  }

  FormatHiiString (
    HiiHandle,
    Language,
    STRING_TOKEN (STR_DECISION_TZ_ERROR),
    Value,
    sizeof (Value),
    Decision,
    NvStatus->ExitGunyahStatus
    );
  SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_DECISION_VALUE), Value);
}

/**
  Fills in where the settings are kept: on UFS when SEC read the store from
  its partition, in memory only otherwise, with the reason.

  @param[in]  HiiHandle  The formset's HII handle.
  @param[in]  Language   The language to fill it in in.
  @param[in]  NvStatus   The status page, NULL if there is none.
**/
STATIC
VOID
UpdateStorage (
  IN EFI_HII_HANDLE                HiiHandle,
  IN CONST CHAR8                   *Language,
  IN CONST QCS6490_NVSTORE_STATUS  *NvStatus  OPTIONAL
  )
{
  CHAR16         Name[GPT_PARTITION_NAME_LENGTH + 1];
  CHAR16         Reason[STATUS_VALUE_LENGTH];
  CHAR16         Value[STATUS_VALUE_LENGTH];
  UINT32         Lun;
  EFI_STRING_ID  Format;

  if (NvStatus == NULL) {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_VALUE_NO_STATUS), Value, sizeof (Value));
    SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_STORAGE_VALUE), Value);
    return;
  }

  GetPartitionName (Name, ARRAY_SIZE (Name));
  Lun = NvStatus->Lun;

  //
  // Without a partition name there is nothing to read or write back, whatever
  // SEC reports for it (NO_PARTITION).
  //
  if (Name[0] == L'\0') {
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_REASON_NO_NAME), Reason, sizeof (Reason));
    FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_STORAGE_MEMORY), Value, sizeof (Value), Reason);
    SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_STORAGE_VALUE), Value);
    return;
  }

  switch (NvStatus->LoadResult) {
    //
    // Memory holds what is on UFS, or a new store that goes there: the FVB
    // driver writes changes back, provided the partition SEC read is the
    // store's.
    //
    case QCS6490_NVSTORE_LOAD_OK:
    case QCS6490_NVSTORE_LOAD_BLANK:
      if (!StatusMatchesStore (NvStatus)) {
        FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_REASON_MISMATCH), Reason, sizeof (Reason));
        break;
      }

      Format = (NvStatus->LoadResult == QCS6490_NVSTORE_LOAD_OK) ? STRING_TOKEN (STR_STORAGE_UFS)
                                                               : STRING_TOKEN (STR_STORAGE_UFS_NEW);
      FormatHiiString (HiiHandle, Language, Format, Value, sizeof (Value), Lun, Name);
      SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_STORAGE_VALUE), Value);
      return;

    case QCS6490_NVSTORE_LOAD_NOT_TRIED:
      FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_REASON_NOT_TRIED), Reason, sizeof (Reason));
      break;

    case QCS6490_NVSTORE_LOAD_NO_PARTITION:
      FormatHiiString (
        HiiHandle,
        Language,
        STRING_TOKEN (STR_REASON_NO_PARTITION),
        Reason,
        sizeof (Reason),
        Lun,
        Name
        );
      break;

    case QCS6490_NVSTORE_LOAD_UFS_ERROR:
      FormatHiiString (
        HiiHandle,
        Language,
        STRING_TOKEN (STR_REASON_UFS_ERROR),
        Reason,
        sizeof (Reason),
        NvStatus->UfsDiagnostic
        );
      break;

    case QCS6490_NVSTORE_LOAD_SMMU_ERROR:
      FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_REASON_SMMU_ERROR), Reason, sizeof (Reason));
      break;

    case QCS6490_NVSTORE_LOAD_TOO_SMALL:
      FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_REASON_TOO_SMALL), Reason, sizeof (Reason), Name);
      break;

    default:
      FormatHiiString (
        HiiHandle,
        Language,
        STRING_TOKEN (STR_REASON_UNKNOWN),
        Reason,
        sizeof (Reason),
        NvStatus->LoadResult
        );
      break;
  }

  FormatHiiString (HiiHandle, Language, STRING_TOKEN (STR_STORAGE_MEMORY), Value, sizeof (Value), Reason);
  SetStatusValue (HiiHandle, Language, STRING_TOKEN (STR_STORAGE_VALUE), Value);
}

/**
  Fills in the status lines of the formset, in every language of its string
  package.

  @param[in]  HiiHandle  The formset's HII handle.
**/
STATIC
VOID
UpdateStatusStrings (
  IN EFI_HII_HANDLE  HiiHandle
  )
{
  QCS6490_NVSTORE_STATUS        StatusPage;
  CONST QCS6490_NVSTORE_STATUS  *NvStatus;
  CHAR8                         *Languages;
  CHAR8                         *Language;
  CHAR8                         *Next;

  NvStatus = NULL;
  if (GetNvStoreStatus (&StatusPage)) {
    NvStatus = &StatusPage;
    DEBUG ((
      DEBUG_INFO,
      "%a: EL%u; SEC: setting %u, xbl_config %u, decision %u, Gunyah exit %u, TrustZone %d, store %u on LUN%u\n",
      __func__,
      (UINT32)(ArmReadCurrentEL () >> 2),
      NvStatus->HypervisorSetting,
      NvStatus->XblOsConfig,
      NvStatus->HypervisorDecision,
      NvStatus->GunyahExit,
      NvStatus->ExitGunyahStatus,
      NvStatus->LoadResult,
      NvStatus->Lun
      ));
  }

  Languages = HiiGetSupportedLanguages (HiiHandle);
  if (Languages == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: no languages\n", __func__));
    return;
  }

  for (Language = Languages; *Language != '\0'; Language = Next) {
    for (Next = Language; (*Next != '\0') && (*Next != ';'); Next++) {
    }

    if (*Next == ';') {
      *Next = '\0';
      Next++;
    }

    //
    // Leave the string package of the keyword protocol alone, like
    // HiiSetString () does.
    //
    if (AsciiStrnCmp (Language, UEFI_CONFIG_LANG, AsciiStrLen (UEFI_CONFIG_LANG)) == 0) {
      continue;
    }

    UpdateExceptionLevel (HiiHandle, Language, NvStatus);
    UpdateXblConfig (HiiHandle, Language, NvStatus);
    UpdateDecision (HiiHandle, Language, NvStatus);
    UpdateStorage (HiiHandle, Language, NvStatus);
  }

  FreePool (Languages);
}

/**
  Creates a setting's variable with its default value when it does not exist
  yet, so that the setup browser can save the formset.

  An existing variable keeps its value. One that config routing could not
  write back, because it has other attributes than the formset declares, is
  written again with the right attributes and its value; one of another size
  is written again with the default, which is also what its readers make of
  it.

  @param[in]  Name     The variable.
  @param[in]  Size     The size of the setting.
  @param[in]  Default  Its default value.
**/
STATIC
VOID
EnsureSetting (
  IN CHAR16      *Name,
  IN UINTN       Size,
  IN CONST VOID  *Default
  )
{
  EFI_STATUS  Status;
  UINT8       Value[16];
  UINTN       ReadSize;
  UINT32      Attributes;

  ASSERT (Size <= sizeof (Value));

  ReadSize   = Size;
  Attributes = 0;
  Status     = gRT->GetVariable (
                      Name,
                      &gQcs6490PlatformConfigGuid,
                      &Attributes,
                      &ReadSize,
                      Value
                      );
  if (!EFI_ERROR (Status) && (ReadSize == Size) && (Attributes == SETTING_ATTRIBUTES)) {
    DEBUG ((DEBUG_INFO, "%a: %s is there\n", __func__, Name));
    return;
  }

  if (Status == EFI_NOT_FOUND) {
    CopyMem (Value, Default, Size);
  } else if (!EFI_ERROR (Status) || (Status == EFI_BUFFER_TOO_SMALL)) {
    if (EFI_ERROR (Status) || (ReadSize != Size)) {
      CopyMem (Value, Default, Size);
    }

    DEBUG ((
      DEBUG_WARN,
      "%a: %s has attributes 0x%x and %lu bytes, writing it again\n",
      __func__,
      Name,
      Attributes,
      (UINT64)ReadSize
      ));

    Status = gRT->SetVariable (
                    Name,
                    &gQcs6490PlatformConfigGuid,
                    0,
                    0,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: cannot delete %s: %r\n", __func__, Name, Status));
      return;
    }
  } else {
    DEBUG ((DEBUG_ERROR, "%a: cannot read %s: %r\n", __func__, Name, Status));
    return;
  }

  Status = gRT->SetVariable (
                  Name,
                  &gQcs6490PlatformConfigGuid,
                  SETTING_ATTRIBUTES,
                  Size,
                  Value
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot create %s: %r\n", __func__, Name, Status));
    return;
  }

  DEBUG ((DEBUG_INFO, "%a: %s created\n", __func__, Name));
}

/**
  Publishes the formset on a new handle with a vendor device path, which
  config routing finds the formset's variable store by.

  @param[out]  HiiHandle  The formset's HII handle.

  @retval EFI_SUCCESS  The formset is in the HII database.
  @retval Other        It could not be added; nothing is left installed.
**/
STATIC
EFI_STATUS
InstallHiiPages (
  OUT EFI_HII_HANDLE  *HiiHandle
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  DriverHandle;

  DriverHandle = NULL;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &DriverHandle,
                        &gEfiDevicePathProtocolGuid,
                        &mVendorDevicePath,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *HiiHandle = HiiAddPackages (
                 &gQcs6490PlatformConfigGuid,
                 DriverHandle,
                 Qcs6490PlatformConfigDxeStrings,
                 PlatformConfigHiiBin,
                 NULL
                 );
  if (*HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mVendorDevicePath,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

/**
  Entry point: makes sure the settings' variables exist, adds the formset and
  fills in its status lines.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The formset is installed.
  @retval Other        It could not be installed.
**/
EFI_STATUS
EFIAPI
PlatformConfigDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  EFI_HII_HANDLE  HiiHandle;

  EnsureSetting (QCS6490_HYPERVISOR_MODE_VARIABLE, sizeof (QCS6490_HYPERVISOR_CONFIG), &mDefaultHypervisorConfig);
  EnsureSetting (QCS6490_DSP_PRELOAD_VARIABLE, sizeof (QCS6490_DSP_PRELOAD_CONFIG), &mDefaultDspPreloadConfig);

  Status = InstallHiiPages (&HiiHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot install the formset: %r\n", __func__, Status));
    return Status;
  }

  UpdateStatusStrings (HiiHandle);

  return EFI_SUCCESS;
}
