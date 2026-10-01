/** @file
  Downloads the firmware of a Renesas uPD720201/uPD720202 xHCI that has no
  EEPROM before XhciDxe drives it.

  These chips run their xHCI core from RAM. Without an EEPROM the firmware is
  downloaded through vendor PCI configuration registers after every power-up;
  until then USBSTS.CNR stays set and the controller does nothing. Linux
  downloads it itself (drivers/usb/host/xhci-pci-renesas.c). When UEFI has
  done it and the chip kept its power, Linux finds the firmware running
  (renesas_fw_check_running() returns 0) and skips both the download and the
  five-second CNR wait of quirk_usb_handoff_xhci().

  The download runs from a driver binding whose Version, 0xFFFFFFF0, sorts it
  before XhciDxe's (0x30) on every ConnectController() of a PCI controller
  (CoreConnectSingleController() tries drivers by Version, highest first).
  Its Supported() downloads when needed and returns EFI_UNSUPPORTED, so that
  XhciDxe binds next. For every other controller it reads the IDs and stops.

  Linux refuses a chip left with FW Download Enable set ("FW Download Enable
  is stale"), with the Lock bit set and no firmware, or with an error result,
  until it loses power. So once Enable has been set, every path clears it.

  When the controller cannot run (no firmware this boot and no EEPROM),
  Supported() returns EFI_SUCCESS and Start() holds PciIo so that XhciDxe does
  not bind: XhciDxe resets the controller and then only ASSERTs that CNR is
  clear (Xhci.c XhcDriverBindingStart()), which hangs a DEBUG build.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "RenesasXhciFw.h"

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DriverBinding.h>

//
// 0xFFFFFFF0-0xFFFFFFFF are for platform drivers; above XhciDxe's 0x30.
//
#define RENESAS_XHCI_FW_DRIVER_VERSION  0xFFFFFFF0

//
// What FW Download Control & Status says, as renesas_fw_check_running()
// reads it (xhci-pci-renesas.c:222-286).
//
typedef enum {
  RenesasFwNeeded,                // no result yet: the firmware is wanted
  RenesasFwRunning,
  RenesasFwStale,                 // FW Download Enable left set
  RenesasFwLocked,                // FW Download Lock set, no firmware
  RenesasFwError,                 // the last download failed
  RenesasFwInvalid                // a reserved result code
} RENESAS_FW_STATE;

STATIC CONST CHAR8  *mFwStateNames[] = {
  "no firmware",
  "running",
  "Download Enable stale",
  "locked without firmware",
  "error",
  "invalid result"
};

//
// Set while a controller is being prepared: connecting UFS for the firmware
// calls every driver's Supported() on the new handles, this one's included.
//
STATIC BOOLEAN  mBusy;

/**
  Reads an 8-bit configuration register.

  @param[in]  PciIo   The controller.
  @param[in]  Offset  The register.

  @return  Its value, or all ones if it could not be read.
**/
STATIC
UINT8
RenesasRead8 (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT32               Offset
  )
{
  UINT8  Value;

  if (EFI_ERROR (PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, Offset, 1, &Value))) {
    return MAX_UINT8;
  }

  return Value;
}

/**
  Reads a 16-bit configuration register.

  @param[in]  PciIo   The controller.
  @param[in]  Offset  The register.

  @return  Its value, or all ones if it could not be read.
**/
STATIC
UINT16
RenesasRead16 (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT32               Offset
  )
{
  UINT16  Value;

  if (EFI_ERROR (PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, Offset, 1, &Value))) {
    return MAX_UINT16;
  }

  return Value;
}

/**
  Reads a 32-bit configuration register.

  @param[in]  PciIo   The controller.
  @param[in]  Offset  The register.

  @return  Its value, or all ones if it could not be read.
**/
STATIC
UINT32
RenesasRead32 (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT32               Offset
  )
{
  UINT32  Value;

  if (EFI_ERROR (PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, Offset, 1, &Value))) {
    return MAX_UINT32;
  }

  return Value;
}

/**
  Writes an 8-bit configuration register, and only that byte.

  @param[in]  PciIo   The controller.
  @param[in]  Offset  The register.
  @param[in]  Value   The value.

  @retval EFI_SUCCESS  Written.
  @retval Other        From PciIo.
**/
STATIC
EFI_STATUS
RenesasWrite8 (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT32               Offset,
  IN UINT8                Value
  )
{
  return PciIo->Pci.Write (PciIo, EfiPciIoWidthUint8, Offset, 1, &Value);
}

/**
  Writes a 32-bit configuration register.

  @param[in]  PciIo   The controller.
  @param[in]  Offset  The register.
  @param[in]  Value   The value.

  @retval EFI_SUCCESS  Written.
  @retval Other        From PciIo.
**/
STATIC
EFI_STATUS
RenesasWrite32 (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT32               Offset,
  IN UINT32               Value
  )
{
  return PciIo->Pci.Write (PciIo, EfiPciIoWidthUint32, Offset, 1, &Value);
}

/**
  Polls an 8-bit configuration register until (value & Mask) == Value, as
  Linux does: RENESAS_POLL_RETRIES reads RENESAS_POLL_DELAY_US apart.

  @param[in]   PciIo   The controller.
  @param[in]   Offset  The register.
  @param[in]   Mask    The bits to look at.
  @param[in]   Value   The value they must have.
  @param[out]  Last    The last value read.

  @retval EFI_SUCCESS  The bits reached the value.
  @retval EFI_TIMEOUT  They did not.
**/
STATIC
EFI_STATUS
RenesasPoll8 (
  IN  EFI_PCI_IO_PROTOCOL  *PciIo,
  IN  UINT32               Offset,
  IN  UINT8                Mask,
  IN  UINT8                Value,
  OUT UINT8                *Last
  )
{
  UINTN  Try;

  for (Try = 0; ; Try++) {
    *Last = RenesasRead8 (PciIo, Offset);
    if ((*Last & Mask) == Value) {
      return EFI_SUCCESS;
    }

    if (Try >= RENESAS_POLL_RETRIES) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (RENESAS_POLL_DELAY_US);
  }
}

/**
  Classifies FW Download Control & Status the way renesas_fw_check_running()
  does (xhci-pci-renesas.c:222-286): Lock first, then Download Enable, then
  the result code.

  @param[in]  FwStatus  Configuration byte 0xF4.

  @return  The state.
**/
STATIC
RENESAS_FW_STATE
RenesasFwState (
  IN UINT8  FwStatus
  )
{
  if ((FwStatus & RENESAS_FW_STATUS_LOCK) != 0) {
    return ((FwStatus & RENESAS_FW_STATUS_RESULT_SUCCESS) != 0) ? RenesasFwRunning : RenesasFwLocked;
  }

  if ((FwStatus & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) != 0) {
    return RenesasFwStale;
  }

  switch (FwStatus & RENESAS_FW_STATUS_RESULT_MASK) {
    case RENESAS_FW_STATUS_RESULT_NONE:
      return RenesasFwNeeded;
    case RENESAS_FW_STATUS_RESULT_SUCCESS:
      return RenesasFwRunning;
    case RENESAS_FW_STATUS_RESULT_ERROR:
      return RenesasFwError;
    default:
      return RenesasFwInvalid;
  }
}

/**
  Ends a download: clears FW Download Enable (step 11 of
  renesas_fw_download(), xhci-pci-renesas.c:340-345) and waits for the
  result code (step 12, :348-357).

  Linux waits for the success bit only. An error result ends the wait here
  too, since the hardware has decided by then.

  @param[in]  PciIo  The controller.

  @return  FW Download Control & Status at the end.
**/
STATIC
UINT8
RenesasFwEndDownload (
  IN EFI_PCI_IO_PROTOCOL  *PciIo
  )
{
  EFI_STATUS  Status;
  UINT8       FwStatus;
  UINT8       Result;
  UINTN       Try;

  Status = RenesasWrite8 (PciIo, RENESAS_FW_STATUS, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: clearing FW Download Enable: %r\n", __func__, Status));
  }

  for (Try = 0; ; Try++) {
    FwStatus = RenesasRead8 (PciIo, RENESAS_FW_STATUS);
    Result   = FwStatus & RENESAS_FW_STATUS_RESULT_MASK;
    if (((FwStatus & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) == 0) &&
        ((Result == RENESAS_FW_STATUS_RESULT_SUCCESS) || (Result == RENESAS_FW_STATUS_RESULT_ERROR)))
    {
      break;
    }

    if (Try >= RENESAS_POLL_RETRIES) {
      break;
    }

    MicroSecondDelay (RENESAS_POLL_DELAY_US);
  }

  //
  // Never hand the chip on with Download Enable set: Linux gives up on it.
  //
  if ((FwStatus & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: FW Download Enable still set (0xF4 = 0x%02x), clearing it again\n", __func__, FwStatus));
    RenesasWrite8 (PciIo, RENESAS_FW_STATUS, 0);
    FwStatus = RenesasRead8 (PciIo, RENESAS_FW_STATUS);
    if ((FwStatus & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) != 0) {
      DEBUG ((DEBUG_ERROR, "%a: FW Download Enable stuck (0xF4 = 0x%02x)\n", __func__, FwStatus));
    }
  }

  return FwStatus;
}

/**
  Downloads the firmware into the controller's RAM, as renesas_fw_download()
  and renesas_fw_download_image() do (xhci-pci-renesas.c:56-120, 288-379),
  except that a failed step still clears FW Download Enable.

  @param[in]   PciIo     The controller.
  @param[in]   Image     The image, checked.
  @param[in]   Size      Its size, a multiple of 4.
  @param[out]  FwStatus  FW Download Control & Status at the end.

  @retval EFI_SUCCESS  Every dword was sent; FwStatus says whether the
                       firmware took.
  @retval Other        A step failed; FwStatus says what was left.
**/
STATIC
EFI_STATUS
RenesasFwDownload (
  IN  EFI_PCI_IO_PROTOCOL  *PciIo,
  IN  CONST UINT8          *Image,
  IN  UINTN                Size,
  OUT UINT8                *FwStatus
  )
{
  EFI_STATUS  Status;
  UINTN       Dwords;
  UINTN       Index;
  UINT8       SetData;
  UINT8       Msb;
  UINT32      Data;
  UINT64      Start;

  Dwords = Size / sizeof (UINT32);
  Index  = 0;
  Start  = GetPerformanceCounter ();

  //
  // Step 0: FW Download Enable.
  //
  Status = RenesasWrite8 (PciIo, RENESAS_FW_STATUS, RENESAS_FW_STATUS_DOWNLOAD_ENABLE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: setting FW Download Enable: %r\n", __func__, Status));
    goto End;
  }

  //
  // Steps 1-10: dword i goes through DATA0 when i is even and DATA1 when it
  // is odd, once the hardware has taken the previous dword from that
  // register ("Set DATAx" clear). "LSB is left": each dword is the image's
  // four bytes read little endian. The image may hold an odd number of
  // dwords (renesas_usb_fw.mem has 3253); the last one then goes alone
  // through DATA0.
  //
  for (Index = 0; Index < Dwords; Index++) {
    SetData = ((Index & 1) == 0) ? RENESAS_FW_STATUS_MSB_SET_DATA0 : RENESAS_FW_STATUS_MSB_SET_DATA1;

    Status = RenesasPoll8 (PciIo, RENESAS_FW_STATUS_MSB, SetData, 0, &Msb);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: dword %lu: Set DATA%u not cleared (0xF5 = 0x%02x, 0xF4 = 0x%02x)\n",
        __func__,
        (UINT64)Index,
        (UINT32)(Index & 1),
        Msb,
        RenesasRead8 (PciIo, RENESAS_FW_STATUS)
        ));
      goto End;
    }

    Data   = ReadUnaligned32 ((CONST UINT32 *)(Image + Index * sizeof (UINT32)));
    Status = RenesasWrite32 (PciIo, ((Index & 1) == 0) ? RENESAS_DATA0 : RENESAS_DATA1, Data);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: dword %lu: writing DATA%u: %r\n", __func__, (UINT64)Index, (UINT32)(Index & 1), Status));
      goto End;
    }

    MicroSecondDelay (RENESAS_DATA_SETTLE_US);

    Status = RenesasWrite8 (PciIo, RENESAS_FW_STATUS_MSB, SetData);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: dword %lu: setting Set DATA%u: %r\n", __func__, (UINT64)Index, (UINT32)(Index & 1), Status));
      goto End;
    }
  }

  //
  // Let the hardware take the last dword(s). Linux only warns when it does
  // not (xhci-pci-renesas.c:327-338); the result code tells.
  //
  if (EFI_ERROR (RenesasPoll8 (PciIo, RENESAS_FW_STATUS_MSB, RENESAS_FW_STATUS_MSB_SET_DATA0 | RENESAS_FW_STATUS_MSB_SET_DATA1, 0, &Msb))) {
    DEBUG ((DEBUG_WARN, "%a: last dwords not taken (0xF5 = 0x%02x)\n", __func__, Msb));
  }

End:
  //
  // Steps 11 and 12, whatever happened above.
  //
  *FwStatus = RenesasFwEndDownload (PciIo);

  DEBUG ((
    DEBUG_INFO,
    "%a: %lu of %lu dwords in %lu ms, 0xF4 = 0x%02x, 0xF5 = 0x%02x\n",
    __func__,
    (UINT64)Index,
    (UINT64)Dwords,
    DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter () - Start), 1000000),
    *FwStatus,
    RenesasRead8 (PciIo, RENESAS_FW_STATUS_MSB)
    ));

  return Status;
}

/**
  Waits for the xHCI to come out of Controller Not Ready once its firmware
  runs, as quirk_usb_handoff_xhci() does (pci-quirks.c:1232-1240), so that
  XhciDxe, which only ASSERTs CNR clear, finds it ready. Memory decoding is
  enabled through PciIo for the wait and the attributes are put back after.

  @param[in]  PciIo  The controller.

  @retval EFI_SUCCESS  CNR is clear.
  @retval EFI_TIMEOUT  CNR stayed set for RENESAS_XHCI_READY_TIMEOUT_US.
  @retval Other        BAR0 could not be read.
**/
STATIC
EFI_STATUS
RenesasXhciWaitReady (
  IN EFI_PCI_IO_PROTOCOL  *PciIo
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  RestoreStatus;
  UINT64      Attributes;
  UINT32      Cap;
  UINT32      CapLength;
  UINT32      UsbSts;
  UINTN       Waited;

  Status = PciIo->Attributes (PciIo, EfiPciIoAttributeOperationGet, 0, &Attributes);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: PCI attributes: %r\n", __func__, Status));
    return Status;
  }

  Cap       = MAX_UINT32;
  CapLength = 0;
  UsbSts    = MAX_UINT32;
  Waited    = 0;

  Status = PciIo->Attributes (PciIo, EfiPciIoAttributeOperationEnable, EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: enabling memory decoding: %r\n", __func__, Status));
    goto Restore;
  }

  Status = PciIo->Mem.Read (PciIo, EfiPciIoWidthUint32, RENESAS_XHCI_BAR_INDEX, XHCI_CAPLENGTH_OFFSET, 1, &Cap);
  if (!EFI_ERROR (Status)) {
    CapLength = Cap & XHCI_CAPLENGTH_MASK;
    if ((Cap == MAX_UINT32) || (CapLength == 0)) {
      Status = EFI_DEVICE_ERROR;
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: BAR0 capabilities 0x%08x: %r\n", __func__, Cap, Status));
    goto Restore;
  }

  for ( ; ; Waited += RENESAS_XHCI_READY_POLL_US) {
    Status = PciIo->Mem.Read (PciIo, EfiPciIoWidthUint32, RENESAS_XHCI_BAR_INDEX, CapLength + XHCI_USBSTS_OFFSET, 1, &UsbSts);
    if (EFI_ERROR (Status) || ((UsbSts & XHCI_USBSTS_CNR) == 0)) {
      break;
    }

    if (Waited >= RENESAS_XHCI_READY_TIMEOUT_US) {
      Status = EFI_TIMEOUT;
      break;
    }

    MicroSecondDelay (RENESAS_XHCI_READY_POLL_US);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: CAPLENGTH 0x%x, USBSTS 0x%08x after %lu us, CNR still set: %r\n",
      __func__,
      CapLength,
      UsbSts,
      (UINT64)Waited,
      Status
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "%a: ready after %lu us: CAPLENGTH 0x%x, USBSTS 0x%08x (HCH %u)\n",
      __func__,
      (UINT64)Waited,
      CapLength,
      UsbSts,
      (UINT32)((UsbSts & XHCI_USBSTS_HCH) != 0)
      ));
  }

Restore:
  RestoreStatus = PciIo->Attributes (PciIo, EfiPciIoAttributeOperationSet, Attributes, NULL);
  if (EFI_ERROR (RestoreStatus)) {
    DEBUG ((DEBUG_ERROR, "%a: restoring PCI attributes 0x%lx: %r\n", __func__, Attributes, RestoreStatus));
  }

  return Status;
}

/**
  Makes the firmware of one controller run if it does not, following
  renesas_xhci_check_request_fw() (xhci-pci-renesas.c:579-628) for the RAM
  download. An EEPROM is never written; with one, the firmware it loaded is
  used, and otherwise the RAM download is tried as Linux falls back to it.

  @param[in]   PciIo   The controller, opened by this driver.
  @param[out]  HasRom  Whether the controller has an EEPROM.

  @retval TRUE   The firmware runs, and the xHCI is ready unless an EEPROM
                 loaded the firmware.
  @retval FALSE  It does not, or the xHCI is not ready, and will not be
                 this boot.
**/
STATIC
BOOLEAN
RenesasXhciPrepare (
  IN  EFI_PCI_IO_PROTOCOL  *PciIo,
  OUT BOOLEAN              *HasRom
  )
{
  EFI_STATUS        Status;
  UINT16            RomStatus;
  UINT32            Version;
  UINT8             FwStatus;
  RENESAS_FW_STATE  State;
  CONST UINT8       *Image;
  UINTN             Size;
  UINT16            ImageVersion;

  RomStatus = RenesasRead16 (PciIo, RENESAS_ROM_STATUS);
  Version   = RenesasRead32 (PciIo, RENESAS_FW_VERSION);
  FwStatus  = RenesasRead8 (PciIo, RENESAS_FW_STATUS);
  State     = RenesasFwState (FwStatus);
  *HasRom   = (RomStatus & RENESAS_ROM_STATUS_ROM_EXISTS) != 0;

  DEBUG ((
    DEBUG_INFO,
    "%a: FW status 0x%02x (%a), ROM status 0x%04x, version %04x (0x%08x)\n",
    __func__,
    FwStatus,
    mFwStateNames[State],
    RomStatus,
    RENESAS_FW_VERSION_OF (Version),
    Version
    ));

  //
  // renesas_check_rom_state(): an EEPROM that has loaded the firmware leaves
  // nothing to do.
  //
  if (*HasRom) {
    if ((RomStatus & RENESAS_ROM_STATUS_RESULT_MASK) == RENESAS_ROM_STATUS_RESULT_SUCCESS) {
      DEBUG ((DEBUG_INFO, "%a: firmware %04x loaded from the EEPROM\n", __func__, RENESAS_FW_VERSION_OF (Version)));
      return TRUE;
    }

    DEBUG ((DEBUG_WARN, "%a: EEPROM without a loaded firmware; it is left alone\n", __func__));
  }

  //
  // Download Enable left set by an earlier boot that kept the chip powered.
  // Linux would give up on it; ending that download may free it.
  //
  if (State == RenesasFwStale) {
    DEBUG ((DEBUG_WARN, "%a: FW Download Enable left set, ending that download\n", __func__));
    FwStatus = RenesasFwEndDownload (PciIo);
    State    = RenesasFwState (FwStatus);
    DEBUG ((DEBUG_INFO, "%a: FW status now 0x%02x (%a)\n", __func__, FwStatus, mFwStateNames[State]));
  }

  switch (State) {
    case RenesasFwRunning:
      DEBUG ((DEBUG_INFO, "%a: firmware %04x already running\n", __func__, RENESAS_FW_VERSION_OF (Version)));
      return !EFI_ERROR (RenesasXhciWaitReady (PciIo));

    case RenesasFwNeeded:
      break;

    default:
      DEBUG ((
        DEBUG_ERROR,
        "%a: no firmware and none can be downloaded: FW status 0x%02x (%a); the chip needs a power cycle\n",
        __func__,
        FwStatus,
        mFwStateNames[State]
        ));
      return FALSE;
  }

  Status = RenesasFirmwareGet (&Image, &Size, &ImageVersion);
  if (EFI_ERROR (Status)) {
    //
    // Reported by RenesasFirmwareGet(), once.
    //
    return FALSE;
  }

  DEBUG ((DEBUG_INFO, "%a: downloading firmware %04x, %lu bytes\n", __func__, ImageVersion, (UINT64)Size));
  Status  = RenesasFwDownload (PciIo, Image, Size, &FwStatus);
  State   = RenesasFwState (FwStatus);
  Version = RenesasRead32 (PciIo, RENESAS_FW_VERSION);

  if (State != RenesasFwRunning) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: download failed (%r): FW status 0x%02x (%a), version 0x%08x\n",
      __func__,
      Status,
      FwStatus,
      mFwStateNames[State],
      Version
      ));
    return FALSE;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: a download step failed (%r), yet the firmware reports success\n", __func__, Status));
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: firmware %04x running (0x%08x), the image carries %04x\n",
    __func__,
    RENESAS_FW_VERSION_OF (Version),
    Version,
    ImageVersion
    ));

  //
  // The firmware stays loaded for Linux whatever the wait gives; but an xHCI
  // that is still not ready after a second, or whose registers cannot be
  // read, would not work under XhciDxe either.
  //
  return !EFI_ERROR (RenesasXhciWaitReady (PciIo));
}

/**
  Tests a controller: a Renesas uPD720201/uPD720202 gets its firmware here.

  For every other controller, and for one XhciDxe already drives, this costs
  an OpenProtocol() and at most one configuration read.

  @param[in]  This                 The driver binding.
  @param[in]  Controller           The controller.
  @param[in]  RemainingDevicePath  Unused.

  @retval EFI_SUCCESS          A Renesas xHCI whose firmware does not run and
                               cannot this boot: Start() holds it.
  @retval EFI_ALREADY_STARTED  This driver holds it already.
  @retval EFI_UNSUPPORTED      Anything else, including a Renesas xHCI ready
                               for XhciDxe.
**/
STATIC
EFI_STATUS
EFIAPI
RenesasXhciFwSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT32               Id;
  UINT16               DeviceId;
  UINTN                Segment;
  UINTN                Bus;
  UINTN                Device;
  UINTN                Function;
  BOOLEAN              Usable;
  BOOLEAN              HasRom;

  if (mBusy) {
    return EFI_UNSUPPORTED;
  }

  //
  // No PciIo, or a driver has the controller: XhciDxe, whose Start() only
  // succeeds once the firmware runs, or this one.
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return (Status == EFI_ALREADY_STARTED) ? EFI_ALREADY_STARTED : EFI_UNSUPPORTED;
  }

  Id       = MAX_UINT32;
  Status   = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_VENDOR_ID_OFFSET, 1, &Id);
  DeviceId = (UINT16)(Id >> 16);
  if (EFI_ERROR (Status) || ((Id & MAX_UINT16) != RENESAS_PCI_VENDOR_ID) ||
      ((DeviceId != RENESAS_PCI_DEVICE_UPD720201) && (DeviceId != RENESAS_PCI_DEVICE_UPD720202)))
  {
    gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
    return EFI_UNSUPPORTED;
  }

  if (EFI_ERROR (PciIo->GetLocation (PciIo, &Segment, &Bus, &Device, &Function))) {
    Segment  = 0;
    Bus      = 0;
    Device   = 0;
    Function = 0;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: %04x:%04x at %04x:%02x:%02x.%x\n",
    __func__,
    RENESAS_PCI_VENDOR_ID,
    DeviceId,
    (UINT32)Segment,
    (UINT32)Bus,
    (UINT32)Device,
    (UINT32)Function
    ));

  mBusy  = TRUE;
  Usable = RenesasXhciPrepare (PciIo, &HasRom);
  mBusy  = FALSE;

  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);

  //
  // With an EEPROM, Linux probes the controller even when no firmware was
  // downloaded (renesas_xhci_check_request_fw() returns 0 when has_rom), and
  // XhciDxe gets the same chance.
  //
  if (Usable || HasRom) {
    return EFI_UNSUPPORTED;
  }

  DEBUG ((DEBUG_WARN, "%a: holding the controller, so that XhciDxe leaves it alone\n", __func__));
  return EFI_SUCCESS;
}

/**
  Holds a Renesas xHCI that has no firmware, so that no other driver binds.

  @param[in]  This                 The driver binding.
  @param[in]  Controller           The controller.
  @param[in]  RemainingDevicePath  Unused.

  @retval EFI_SUCCESS  Held.
  @retval Other        From OpenProtocol().
**/
STATIC
EFI_STATUS
EFIAPI
RenesasXhciFwStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "%a: %p: %r\n", __func__, Controller, Status));
  return Status;
}

/**
  Lets go of a controller held by Start().

  @param[in]  This               The driver binding.
  @param[in]  Controller         The controller.
  @param[in]  NumberOfChildren   None are made.
  @param[in]  ChildHandleBuffer  Unused.

  @retval EFI_SUCCESS  Released.
**/
STATIC
EFI_STATUS
EFIAPI
RenesasXhciFwStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  return EFI_SUCCESS;
}

STATIC EFI_UNICODE_STRING_TABLE  mDriverNameTable[] = {
  { "en", L"Renesas uPD720201/uPD720202 xHCI Firmware Loader" },
  { NULL, NULL }
};

/**
  Returns the driver's name.

  @param[in]   This        The component name protocol.
  @param[in]   Language    An RFC 4646 language code.
  @param[out]  DriverName  The name.

  @retval EFI_SUCCESS      Found.
  @retval EFI_UNSUPPORTED  Not in that language.
**/
STATIC
EFI_STATUS
EFIAPI
RenesasXhciFwGetDriverName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **DriverName
  )
{
  return LookupUnicodeString2 (Language, This->SupportedLanguages, mDriverNameTable, DriverName, FALSE);
}

/**
  Controllers have no name of this driver's.

  @param[in]   This              The component name protocol.
  @param[in]   ControllerHandle  The controller.
  @param[in]   ChildHandle       A child.
  @param[in]   Language          An RFC 4646 language code.
  @param[out]  ControllerName    The name.

  @retval EFI_UNSUPPORTED  Always.
**/
STATIC
EFI_STATUS
EFIAPI
RenesasXhciFwGetControllerName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  EFI_HANDLE                    ControllerHandle,
  IN  EFI_HANDLE                    ChildHandle        OPTIONAL,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **ControllerName
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_COMPONENT_NAME2_PROTOCOL  mComponentName2 = {
  RenesasXhciFwGetDriverName,
  RenesasXhciFwGetControllerName,
  "en"
};

STATIC EFI_DRIVER_BINDING_PROTOCOL  mDriverBinding = {
  RenesasXhciFwSupported,
  RenesasXhciFwStart,
  RenesasXhciFwStop,
  RENESAS_XHCI_FW_DRIVER_VERSION,
  NULL,
  NULL
};

/**
  Entry point: installs the driver binding.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  Installed.
  @retval Other        It could not be.
**/
EFI_STATUS
EFIAPI
RenesasXhciFwDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mDriverBinding,
           ImageHandle,
           NULL,
           &mComponentName2
           );
}
