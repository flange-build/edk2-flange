#
#  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
#  Shared by the QCS6490 boards. Derived from the upstream edk2-platforms
#  RB3 Gen 2 port (Platform/Qualcomm/RB3Gen2/RB3Gen2.dsc).
#
#  A board DSC defines PLATFORM_NAME, PLATFORM_DIRECTORY and friends, includes
#  this file, then adds its SMBIOS strings and its device tree component.
#

  #
  # Defines for default states.  These can be changed on the command line.
  # -D FLAG=VALUE
  #
  DEFINE DEBUG_PRINT_ERROR_LEVEL = 0x8000004F
  DEFINE FIRMWARE_VER            = 0.1

  #
  # Tear down Gunyah and run UEFI and the OS at EL2 (see PcdExitGunyah) when
  # neither the Hypervisor setting nor xbl_config says which.
  #
  DEFINE EXIT_GUNYAH             = FALSE

  #
  # Language the menus start in until one is picked under Select Language
  # on the front page: en-US or zh-Hans.
  #
  DEFINE DEFAULT_LANG            = en-US

################################################################################
#
# Library Class section - list of all Library Classes needed by this Platform.
#
################################################################################

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses.common]
!if $(TARGET) == RELEASE
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
!else
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
!endif
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsicArmVirt.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf

  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLibDevicePathProtocol/UefiDevicePathLibDevicePathProtocol.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf

  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf

  #
  # Ramdisk Requirements
  #
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf

  # Allow dynamic PCDs
  #
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf

  # use the accelerated BaseMemoryLibOptDxe by default, overrides for SEC/PEI below
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf

  # ARM Architectural Libraries
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmSmcccSocIdLib|ArmPkg/Library/ArmSmcccSocIdLib/ArmSmcccSocIdLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmTransferListLib|ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerVirtCounterLib/ArmGenericTimerVirtCounterLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf

  PlatformPeiLib|ArmPlatformPkg/PlatformPei/PlatformPeiLib.inf
  MemoryInitPeiLib|Silicon/Qualcomm/QCS6490/Library/MemoryInitPeiLib/MemoryInitPeiLib.inf
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf

  # Qcom GENI UART Library
  SerialPortLib|Silicon/Qualcomm/Library/GeniSerialPortLib/GeniSerialPortLib.inf

  # Qualcomm shared memory, for the RAM partition table
  QualcommSmemLib|Silicon/Qualcomm/QualcommSiliconPkg/Library/QualcommSmemLib/QualcommSmemLib.inf

  # RPMh votes through the apps RSC (display, DSP preload)
  Qcs6490RpmhLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490RpmhLib/Qcs6490RpmhLib.inf

  # What SEC found and decided (exception level, variable store)
  Qcs6490NvStatusLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490NvStatusLib/Qcs6490NvStatusLib.inf

  # GCC clocks and TLMM GPIOs (PCIe, USB)
  Qcs6490GccLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490GccLib/Qcs6490GccLib.inf
  Qcs6490TlmmLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490TlmmLib/Qcs6490TlmmLib.inf

  # The PCIe0 root complex, for PciHostBridgeDxe
  PciHostBridgeLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490PciHostBridgeLib/Qcs6490PciHostBridgeLib.inf
  PciSegmentLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490PciSegmentLib/Qcs6490PciSegmentLib.inf

  # USB class drivers
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf

  # Device trees (DSP preload)
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  RealTimeClockLib|EmbeddedPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf

  # SMBIOS platform information
  OemMiscLib|Silicon/Qualcomm/QCS6490/Library/OemMiscLib/OemMiscLib.inf

  # Debug Support
  PeCoffExtraActionLib|ArmPkg/Library/DebugPeCoffExtraActionLib/DebugPeCoffExtraActionLib.inf
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  DebugAgentTimerLib|EmbeddedPkg/Library/DebugAgentTimerLibNull/DebugAgentTimerLibNull.inf

  #
  # CryptoPkg libraries needed by multiple firmware features
  #
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLib.inf
  RngLib|MdeModulePkg/Library/BaseRngLibTimerLib/BaseRngLibTimerLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf

  #
  # Secure boot NOP library classes
  #
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf

  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf

  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf

  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf

  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  ArmPlatformLib|Silicon/Qualcomm/QCS6490/Library/Qcs6490Lib/Qcs6490Lib.inf

  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  PlatformBootManagerLib|ArmPkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf


[LibraryClasses.common.SEC]
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf

  DebugAgentLib|ArmPkg/Library/DebugAgentSymbolsBaseLib/DebugAgentSymbolsBaseLib.inf
  HobLib|MdePkg/Library/PeiHobLib/PeiHobLib.inf
  PeiServicesLib|MdePkg/Library/PeiServicesLib/PeiServicesLib.inf
  PeiServicesTablePointerLib|ArmPkg/Library/PeiServicesTablePointerLib/PeiServicesTablePointerLib.inf
  MemoryAllocationLib|MdePkg/Library/PeiMemoryAllocationLib/PeiMemoryAllocationLib.inf

[LibraryClasses.common.PEI_CORE]
  PcdLib|MdePkg/Library/PeiPcdLib/PeiPcdLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  HobLib|MdePkg/Library/PeiHobLib/PeiHobLib.inf
  PeiServicesLib|MdePkg/Library/PeiServicesLib/PeiServicesLib.inf
  MemoryAllocationLib|MdePkg/Library/PeiMemoryAllocationLib/PeiMemoryAllocationLib.inf
  PeiCoreEntryPoint|MdePkg/Library/PeiCoreEntryPoint/PeiCoreEntryPoint.inf
  PerformanceLib|MdeModulePkg/Library/PeiPerformanceLib/PeiPerformanceLib.inf
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/PeiExtractGuidedSectionLib/PeiExtractGuidedSectionLib.inf

  PeiServicesTablePointerLib|ArmPkg/Library/PeiServicesTablePointerLib/PeiServicesTablePointerLib.inf

[LibraryClasses.common.PEIM]
  PcdLib|MdePkg/Library/PeiPcdLib/PeiPcdLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  HobLib|MdePkg/Library/PeiHobLib/PeiHobLib.inf
  PeiServicesLib|MdePkg/Library/PeiServicesLib/PeiServicesLib.inf
  MemoryAllocationLib|MdePkg/Library/PeiMemoryAllocationLib/PeiMemoryAllocationLib.inf
  PeimEntryPoint|MdePkg/Library/PeimEntryPoint/PeimEntryPoint.inf
  PerformanceLib|MdeModulePkg/Library/PeiPerformanceLib/PeiPerformanceLib.inf
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  ResourcePublicationLib|MdePkg/Library/PeiResourcePublicationLib/PeiResourcePublicationLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/PeiExtractGuidedSectionLib/PeiExtractGuidedSectionLib.inf

  PeiServicesTablePointerLib|ArmPkg/Library/PeiServicesTablePointerLib/PeiServicesTablePointerLib.inf

[LibraryClasses.common.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  PerformanceLib|MdeModulePkg/Library/DxeCorePerformanceLib/DxeCorePerformanceLib.inf

[LibraryClasses.common.DXE_DRIVER]
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf

[LibraryClasses.common.UEFI_APPLICATION]
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf

[LibraryClasses.common.UEFI_DRIVER]
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf
!if $(TARGET) != RELEASE
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf
!endif

  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/RuntimeCryptLib.inf

###################################################################################################
# BuildOptions Section - Define the module specific tool chain flags that should be used as
#                        the default flags for a module. These flags are appended to any
#                        standard flags that are defined by the build process.
###################################################################################################

[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  GCC:*_*_AARCH64_DLINK_FLAGS = -z common-page-size=0x10000

[BuildOptions]
  GCC:RELEASE_*_*_CC_FLAGS  = -DMDEPKG_NDEBUG

  #
  # Keep __symbols__ so that the OS can apply overlays to the device tree
  # we hand over, and quieten the warnings about upstream Qualcomm device
  # trees that dtc emits by default.
  #
  *_*_*_DTC_FLAGS = -@ -Wno-unique_unit_address -Wno-avoid_unnecessary_addr_size -Wno-graph_child_address

################################################################################
#
# Pcd Section - list of all EDK II PCD Entries defined by this Platform
#
################################################################################

[PcdsFeatureFlag.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdHiiOsRuntimeSupport|FALSE

  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|TRUE

  #
  # No legacy (SMM) USB support to take xHCI ownership from.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdTurnOffUsbLegacySupport|TRUE

[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdMaximumUnicodeStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumAsciiStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumLinkedListLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdSpinLockTimeout|10000000
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0xAF
  gEfiMdePkgTokenSpaceGuid.PcdPostCodePropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|320

  # DEBUG_ASSERT_ENABLED       0x01
  # DEBUG_PRINT_ENABLED        0x02
  # DEBUG_CODE_ENABLED         0x04
  # CLEAR_MEMORY_ENABLED       0x08
  # ASSERT_BREAKPOINT_ENABLED  0x10
  # ASSERT_DEADLOOP_ENABLED    0x20
!if $(TARGET) == RELEASE
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x21
!else
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
!endif

  #  DEBUG_INIT      0x00000001  // Initialization
  #  DEBUG_WARN      0x00000002  // Warnings
  #  DEBUG_LOAD      0x00000004  // Load events
  #  DEBUG_FS        0x00000008  // EFI File system
  #  DEBUG_POOL      0x00000010  // Alloc & Free (pool)
  #  DEBUG_PAGE      0x00000020  // Alloc & Free (page)
  #  DEBUG_INFO      0x00000040  // Informational debug messages
  #  DEBUG_DISPATCH  0x00000080  // PEI/DXE/SMM Dispatchers
  #  DEBUG_VARIABLE  0x00000100  // Variable
  #  DEBUG_BM        0x00000400  // Boot Manager
  #  DEBUG_BLKIO     0x00001000  // BlkIo Driver
  #  DEBUG_NET       0x00004000  // SNP Driver
  #  DEBUG_UNDI      0x00010000  // UNDI Driver
  #  DEBUG_LOADFILE  0x00020000  // LoadFile
  #  DEBUG_EVENT     0x00080000  // Event messages
  #  DEBUG_GCD       0x00100000  // Global Coherency Database changes
  #  DEBUG_CACHE     0x00200000  // Memory range cachability changes
  #  DEBUG_VERBOSE   0x00400000  // Detailed debug messages that may
  #                              // significantly impact boot performance
  #  DEBUG_ERROR     0x80000000  // Error
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|$(DEBUG_PRINT_ERROR_LEVEL)

  #
  # Optional feature to help prevent EFI memory map fragments
  # Turned on and off via: PcdPrePiProduceMemoryTypeInformationHob
  # Values are in EFI Pages (4K). DXE Core will make sure that
  # at least this much of each type of memory can be allocated
  # from a single memory range. This way you only end up with
  # maximum of two fragements for each type in the memory map
  # (the memory used, and the free memory that was prereserved
  # but not used).
  #
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiReservedMemoryType|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|600
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|400
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesCode|1500
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesData|12000
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderCode|20
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderData|0

  #
  # Enable strict image permissions for all images. (This applies
  # only to images that were built with >= 4 KB section alignment.)
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdImageProtectionPolicy|0x3

  #
  # Enable NX memory protection for all non-code regions, including OEM and OS
  # reserved ones.
  # By passing --pcd PcdDxeNxMemoryProtectionPolicy=0xC000000000007FD1 on the
  # build command line you can allow code execution in EfiLoaderData. This is
  # required when using some outdated GRUB versions.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdDxeNxMemoryProtectionPolicy|0xC000000000007FD5

  #
  # Enable the non-executable DXE stack. (This gets set up by DxeIpl)
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetNxForStack|TRUE

  #
  # System Memory Base -- fixed.
  #
  # The size only sets the window at whose top MemoryInitPeim places the
  # permanent PEI memory (0xDC000000-0xE0000000, which no carve-out touches);
  # the DRAM the OS gets is read from SMEM by Qcs6490Lib.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x60000000

  # CPU information
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9ff00000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800

  #
  # UEFI variable store: variables, FTW working and FTW spare, read from UFS
  # (PcdNvStoreUfsLun / PcdNvStorePartitionName, set by the board) into this
  # memory by SEC, written back by NvStoreFvbDxe. See Include/Qcs6490NvStore.h.
  # It sits in the part of the load window XBL gives UEFI that the FD leaves
  # free, and PEI keeps it for the runtime.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0xA0000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x00040000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0xA0040000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x00010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0xA0050000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x00040000

  # Size of the region used by UEFI in permanent memory (Reserved 64MB)
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x04000000

  ## Default Terminal Type
  ## 0-PCANSI, 1-VT100, 2-VT00+, 3-UTF8, 4-TTYTERM
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4

  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0xd4, 0xb7, 0xc6, 0x80, 0xd8, 0x83, 0x41, 0x49, 0xab, 0x92, 0xb6, 0xb5, 0xd2, 0xda, 0x54, 0x35 }

  #
  # The maximum physical I/O addressability of the processor, set with
  # BuildCpuHob().
  #
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|16

  # Qcom GENI - Serial Terminal (QUP0 SE5, uart5)
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x994000

  # SMEM, where XBL leaves the RAM partition table
  gQualcommSiliconPkgTokenSpaceGuid.PcdSmemBaseAddress|0x80900000
  gQualcommSiliconPkgTokenSpaceGuid.PcdSmemSize|0x200000
  gQualcommSiliconPkgTokenSpaceGuid.PcdSmemMaxItems|512

!if $(EXIT_GUNYAH) == TRUE
  gQcs6490TokenSpaceGuid.PcdExitGunyah|TRUE
!endif

  # Ufs
  gQcomPlatformTokenSpaceGuid.PcdQcomUfsHcDxeBaseAddress|0x1D84000
  gQcomPlatformTokenSpaceGuid.PcdQcomUfsHcDxeSize|0x3000

  gArmTokenSpaceGuid.PcdProcessorManufacturer|L"Qualcomm"
  gArmTokenSpaceGuid.PcdProcessorVersion|L"QCS6490"
  gArmTokenSpaceGuid.PcdProcessorSerialNumber|L"SN0000"
  gArmTokenSpaceGuid.PcdProcessorAssetTag|L"AT0000"
  gArmTokenSpaceGuid.PcdProcessorPartNumber|L"PN0000"

  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"edk2-flange"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"$(FIRMWARE_VER)"

  # Default platform supported RFC 4646 languages: (American) English and
  # Simplified Chinese. LangCodes lists the same languages in ISO 639-2 and
  # in the same order, which the variable driver relies on to keep Lang in
  # step with PlatformLang.
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultPlatformLangCodes|"en-US;zh-Hans"
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultLangCodes|"engzho"
!if "$(DEFAULT_LANG)" == "zh-Hans"
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultPlatformLang|"zh-Hans"
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultLang|"zho"
!elseif "$(DEFAULT_LANG)" != "en-US"
  !error "DEFAULT_LANG must be en-US or zh-Hans"
!endif

[PcdsDynamicHii.common.DEFAULT]
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|L"Timeout"|gEfiGlobalVariableGuid|0x0|5 # Variable: L"Timeout"
  #
  # Connect every device before booting, so that the UFS LUNs get boot
  # options of their own, as on the Rockchip platforms.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootDiscoveryPolicy|L"BootDiscoveryPolicy"|gBootDiscoveryPolicyMgrFormsetGuid|0x0|2 # Connect All

[PcdsDynamicDefault.common]
  #
  # ARM General Interrupt Controller
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x17a00000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x17a60000

  #
  # SMBIOS entry point version
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosVersion|0x0307
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosDocRev|0x0

  gArmTokenSpaceGuid.PcdSystemBiosRelease|0x0100
  gArmTokenSpaceGuid.PcdEmbeddedControllerFirmwareRelease|0x0100

  gQcomKodiakPlatformTokenSpaceGuid.PcdSystemFamily|L"QCS6490"

  gQcomKodiakPlatformTokenSpaceGuid.PcdPlatformVersionMajor|0x0
  gQcomKodiakPlatformTokenSpaceGuid.PcdPlatformVersionMinor|0x0

  #
  # Console: the GOP's own resolution, and the text mode the Rockchip
  # platforms use, which the serial terminal has too.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|100
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|31
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|100
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|31

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform
#
################################################################################

[Components.common]
  #
  # Ramdisk support
  #
  MdeModulePkg/Universal/Disk/RamDiskDxe/RamDiskDxe.inf

  #
  # UEFI application (Shell Embedded Boot Loader)
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
      ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf

    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xFF
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|8000
  }

  #
  # PEI Phase modules
  #
  ArmPlatformPkg/Sec/Sec.inf
  MdeModulePkg/Core/Pei/PeiMain.inf
  MdeModulePkg/Universal/PCD/Pei/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  ArmPlatformPkg/PlatformPei/PlatformPeim.inf
  ArmPlatformPkg/MemoryInitPei/MemoryInitPeim.inf
  ArmPkg/Drivers/CpuPei/CpuPei.inf


  MdeModulePkg/Core/DxeIplPeim/DxeIpl.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  #
  # DXE
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  }
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }

  #
  # Architectural Protocols
  #
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  Silicon/Qualcomm/QCS6490/Drivers/NvStoreFvbDxe/NvStoreFvbDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      # Depex on gEdkiiNvVarStoreFormattedGuid, installed by NvStoreFvbDxe
      NULL|EmbeddedPkg/Library/NvVarStoreFormattedLib/NvVarStoreFormattedLib.inf
  }
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
      NULL|SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.inf
  }
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf

  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  edk2-rockchip/Silicon/Rockchip/Drivers/CjkFontDxe/CjkFontDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf

  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf

  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf

  #
  # FAT filesystem + GPT/MBR partitioning
  #
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
  MdeModulePkg/Universal/Disk/UdfDxe/UdfDxe.inf

  #
  # ext4, read only: the OS's root file system, where DspPreloadDxe reads the
  # DSP firmware from
  #
!include Features/Ext4Pkg/Ext4.dsc.inc

  #
  # Bds
  #
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf {
    <LibraryClasses>
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  MdeModulePkg/Universal/BootManagerPolicyDxe/BootManagerPolicyDxe.inf
  edk2-common/Drivers/LogoDxe/LogoDxe.inf
  edk2-common/Applications/FlangeUi/FlangeUi.inf {
    <LibraryClasses>
      LvglLib|edk2-common/ThirdParty/LvglLib.inf
  }
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootDiscoveryPolicyUiLib/BootDiscoveryPolicyUiLib.inf
  }

  #
  # DTB support. The board adds the device tree itself.
  #
  EmbeddedPkg/Drivers/DtPlatformDxe/DtPlatformDxe.inf {
    <LibraryClasses>
      DtPlatformDtbLoaderLib|EmbeddedPkg/Library/DxeDtPlatformDtbLoaderLibDefault/DxeDtPlatformDtbLoaderLibDefault.inf
  }

  #
  # Generic non-discoverable pcie used for many soc devices
  #
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf

  #
  # Ufs, and the SMMU set up for its DMA under Gunyah
  #
  Silicon/Qualcomm/QCS6490/Drivers/SmmuDxe/SmmuDxe.inf
  Silicon/Qualcomm/Drivers/QcomUfsHcDxe/QcomUfsHcDxe.inf
  MdeModulePkg/Bus/Pci/UfsPciHcDxe/UfsPciHcDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

  #
  # SMBIOS support
  #
  ArmPkg/Universal/Smbios/ProcessorSubClassDxe/ProcessorSubClassDxe.inf
  ArmPkg/Universal/Smbios/SmbiosMiscDxe/SmbiosMiscDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  Silicon/Qualcomm/QCS6490/Drivers/SmbiosMemoryDxe/SmbiosMemoryDxe.inf

  #
  # QUP serial engine firmware for the OS
  #
  Silicon/Qualcomm/QCS6490/Drivers/QupFwDxe/QupFwDxe.inf

  #
  # HDMI through an LT9611 on DSI0, for boards that set PcdDisplayEnable
  #
  Silicon/Qualcomm/QCS6490/Drivers/MdssDisplayDxe/MdssDisplayDxe.inf

  #
  # Platform Configuration in the Device Manager (Hypervisor: EL1 or EL2)
  #
  Silicon/Qualcomm/QCS6490/Drivers/PlatformConfigDxe/PlatformConfigDxe.inf

  #
  # ADSP and CDSP started before the OS, for a kernel that attaches to them
  #
  Silicon/Qualcomm/QCS6490/Drivers/DspPreloadDxe/DspPreloadDxe.inf

  #
  # Leaves Gunyah for EL2 at ExitBootServices, after the DSPs were started
  #
  Silicon/Qualcomm/QCS6490/Drivers/GunyahExitDxe/GunyahExitDxe.inf

  #
  # PCIe0, for boards that set PcdPcie0Enable. The root complex is brought up
  # by Qcs6490PciHostBridgeLib. Its DMA is treated as non-coherent, as the
  # UFS's is, and kept below 4 GiB.
  #
  ArmPkg/Drivers/ArmPciCpuIo2Dxe/ArmPciCpuIo2Dxe.inf
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  EmbeddedPkg/Drivers/NonCoherentIoMmuDxe/NonCoherentIoMmuDxe.inf {
    <PcdsFixedAtBuild>
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xFFFFFFFF
  }

  #
  # USB host: the secondary DWC3 (usb_2) for boards that set
  # PcdUsbSecondaryHostEnable, and xHCI controllers on PCIe, among them a
  # Renesas uPD720201/2 whose firmware is loaded from a partition.
  #
  Silicon/Qualcomm/QCS6490/Drivers/Dwc3HostDxe/Dwc3HostDxe.inf
  Silicon/Qualcomm/QCS6490/Drivers/RenesasXhciFwDxe/RenesasXhciFwDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
