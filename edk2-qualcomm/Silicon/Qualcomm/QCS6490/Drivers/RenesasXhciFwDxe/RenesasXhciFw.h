/** @file
  Renesas uPD720201/uPD720202 xHCI firmware download: the vendor PCI
  configuration registers, the limits Linux applies, and the interface
  between the driver binding and the firmware reader.

  The register layout and every value here come from Linux
  drivers/usb/host/xhci-pci-renesas.c (7.0.2), which cites Renesas
  R19UH0078EJ0500 Rev.5.00, "6.3 Data Format" and "7.1 FW Download
  Interface".

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RENESAS_XHCI_FW_H_
#define RENESAS_XHCI_FW_H_

#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Protocol/PciIo.h>

#define RENESAS_PCI_VENDOR_ID         0x1912
#define RENESAS_PCI_DEVICE_UPD720201  0x0014
#define RENESAS_PCI_DEVICE_UPD720202  0x0015

//
// Vendor PCI configuration registers (xhci-pci-renesas.c:15-48). Each one is
// accessed with exactly the width given: 0xF4 and 0xF5 are separate byte
// registers, and a wider read-modify-write of one would rewrite the other.
//

//
// 32-bit. Firmware version in bits 23:8 (Linux masks GENMASK(23, 7) and
// shifts by 8), e.g. 0x00202609 for firmware 2.0.2.6.
//
#define RENESAS_FW_VERSION        0x6C
#define RENESAS_FW_VERSION_FIELD  0x00FFFF80
#define RENESAS_FW_VERSION_SHIFT  8
#define RENESAS_FW_VERSION_OF(x)  (((x) & RENESAS_FW_VERSION_FIELD) >> RENESAS_FW_VERSION_SHIFT)

//
// 8-bit. FW Download Control & Status.
//
#define RENESAS_FW_STATUS                  0xF4
#define RENESAS_FW_STATUS_DOWNLOAD_ENABLE  BIT0
#define RENESAS_FW_STATUS_LOCK             BIT1
#define RENESAS_FW_STATUS_RESULT_MASK      (BIT6 | BIT5 | BIT4)
#define RENESAS_FW_STATUS_RESULT_NONE      0
#define RENESAS_FW_STATUS_RESULT_SUCCESS   BIT4
#define RENESAS_FW_STATUS_RESULT_ERROR     BIT5

//
// 8-bit. Bits 9:8 of the FW Download Control & Status dword: software sets
// "Set DATAx" once DATAx holds the next dword, the hardware clears it when it
// has taken the dword.
//
#define RENESAS_FW_STATUS_MSB            0xF5
#define RENESAS_FW_STATUS_MSB_SET_DATA0  BIT0
#define RENESAS_FW_STATUS_MSB_SET_DATA1  BIT1

//
// 16-bit. External ROM Access Control & Status.
//
#define RENESAS_ROM_STATUS                 0xF6
#define RENESAS_ROM_STATUS_RESULT_MASK     (BIT6 | BIT5 | BIT4)
#define RENESAS_ROM_STATUS_RESULT_SUCCESS  BIT4
#define RENESAS_ROM_STATUS_ROM_EXISTS      BIT15

//
// 32-bit. Firmware dwords with an even index go to DATA0, odd ones to DATA1.
//
#define RENESAS_DATA0  0xF8
#define RENESAS_DATA1  0xFC

//
// Polling, as Linux does it: RENESAS_RETRY reads RENESAS_DELAY us apart
// (about 500 ms), and 100 us between writing DATAx and setting "Set DATAx"
// (xhci-pci-renesas.c:50-52, 109).
//
#define RENESAS_POLL_DELAY_US   10
#define RENESAS_POLL_RETRIES    50000
#define RENESAS_DATA_SETTLE_US  100

//
// Image checks of renesas_fw_verify() (xhci-pci-renesas.c:122-156): at least
// 4 KiB and less than 64 KiB, "55AA" first, and a version pointer at offset 4
// whose 16-bit version lies inside the image.
//
#define RENESAS_FW_MIN_SIZE                0x1000
#define RENESAS_FW_MAX_SIZE                0x10000
#define RENESAS_FW_SIGNATURE               0x55AA
#define RENESAS_FW_VERSION_POINTER_OFFSET  4

//
// xHCI registers in BAR0 (xHCI 1.2 section 5.3 and 5.4): CAPLENGTH is the low
// byte of the first dword, USBSTS the second operational register.
// USBSTS.CNR (Controller Not Ready) stays set until the firmware runs.
//
#define RENESAS_XHCI_BAR_INDEX         0
#define XHCI_CAPLENGTH_OFFSET          0x00
#define XHCI_CAPLENGTH_MASK            0xFF
#define XHCI_USBSTS_OFFSET             0x04
#define XHCI_USBSTS_HCH                BIT0
#define XHCI_USBSTS_CNR                BIT11
#define RENESAS_XHCI_READY_POLL_US     10
#define RENESAS_XHCI_READY_TIMEOUT_US  1000000

/**
  Returns the firmware image, read from the file PcdRenesasXhciFwPath on the
  GPT partition named PcdRenesasXhciFwPartition and checked as Linux checks
  it.

  The first call reads it, connecting the UFS host first if the partition is
  not there yet. The image is kept for the rest of the boot, and so is a
  failure: it is reported once, and later calls return the same error at
  once.

  @param[out]  Image    The image. Owned by this module; never freed.
  @param[out]  Size     Its size in bytes, a multiple of 4.
  @param[out]  Version  The firmware version the image carries.

  @retval EFI_SUCCESS    The image is there.
  @retval EFI_NOT_READY  Called while the image is being read.
  @retval Other          There is no usable image this boot.
**/
EFI_STATUS
RenesasFirmwareGet (
  OUT CONST UINT8  **Image,
  OUT UINTN        *Size,
  OUT UINT16       *Version
  );

#endif // RENESAS_XHCI_FW_H_
