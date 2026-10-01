#
#  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#  Copyright (c) 2026, edk2-flange contributors.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
#  Thundercomm RUBIK Pi 3 (Qualcomm QCS6490).
#

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = RubikPi3
  PLATFORM_VENDOR                = Thundercomm
  PLATFORM_GUID                  = faf8f33b-65f6-4230-96f7-910f13c67d51
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  VENDOR_DIRECTORY               = Platform/$(PLATFORM_VENDOR)
  PLATFORM_DIRECTORY             = $(VENDOR_DIRECTORY)/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Silicon/Qualcomm/QCS6490/QCS6490.fdf

  #
  # QCS6490-based platform
  #
!include Silicon/Qualcomm/QCS6490/QCS6490.dsc.inc

################################################################################
#
# Pcd Section - list of all EDK II PCD Entries defined by this Platform.
#
################################################################################

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemProductName|L"RUBIK Pi 3"
  gArmTokenSpaceGuid.PcdSystemVersion|L"1.0"
  gArmTokenSpaceGuid.PcdBaseBoardManufacturer|L"Thundercomm"
  gArmTokenSpaceGuid.PcdBaseBoardProductName|L"RUBIK Pi 3"
  gArmTokenSpaceGuid.PcdBaseBoardVersion|L"1.0"

[PcdsDynamicDefault.common]
  gQcomKodiakPlatformTokenSpaceGuid.PcdSystemManufacturer|L"Thundercomm"
  gQcomKodiakPlatformTokenSpaceGuid.PcdSystemSerialNumber|L"SN0000"
  gQcomKodiakPlatformTokenSpaceGuid.PcdSystemSKU|L"SK0000"

  gQcomKodiakPlatformTokenSpaceGuid.PcdBaseBoardAssetTag|L"AT0000"
  gQcomKodiakPlatformTokenSpaceGuid.PcdBaseBoardSerialNumber|L"SN0000"
  gQcomKodiakPlatformTokenSpaceGuid.PcdBaseBoardSKU|L"SK000"
  gQcomKodiakPlatformTokenSpaceGuid.PcdBaseBoardLocation|L"Public"

  gQcomKodiakPlatformTokenSpaceGuid.PcdChassisSerialNumber|L"SN0000"
  gQcomKodiakPlatformTokenSpaceGuid.PcdChassisVersion|L"1.0"
  gQcomKodiakPlatformTokenSpaceGuid.PcdChassisManufacturer|L"Thundercomm"
  gQcomKodiakPlatformTokenSpaceGuid.PcdChassisAssetTag|L"AT0000"
  gQcomKodiakPlatformTokenSpaceGuid.PcdChassisSKU|L"SK0000"

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform
#
################################################################################

[Components.common]
  #
  # Mainline device tree (devicetree-rebasing), handed over to the OS by
  # DtPlatformDxe. GRUB's "devicetree" command replaces it.
  #
  $(PLATFORM_DIRECTORY)/DeviceTree/Mainline.inf
