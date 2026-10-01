/** @file
  Marker protocol, installed with no interface once SmmuDxe has set up the
  apps SMMU streams of the devices UEFI does DMA with. Drivers that do DMA
  depend on it.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_SMMU_READY_H_
#define QCS6490_SMMU_READY_H_

#define QCS6490_SMMU_READY_PROTOCOL_GUID \
  { 0xfc576c2b, 0xcb3c, 0x44ec, { 0x82, 0x5a, 0xea, 0xdc, 0x52, 0xdc, 0xcb, 0x69 } }

extern EFI_GUID  gQcs6490SmmuReadyProtocolGuid;

#endif // QCS6490_SMMU_READY_H_
