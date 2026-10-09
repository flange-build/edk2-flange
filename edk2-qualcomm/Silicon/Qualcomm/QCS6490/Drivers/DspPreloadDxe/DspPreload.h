/** @file
  QCS6490 DSP preload: loads the ADSP and CDSP firmware and starts the DSPs
  through TrustZone (PAS), for a kernel that attaches to running DSPs.

  Interfaces between the parts of the driver.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef DSP_PRELOAD_H_
#define DSP_PRELOAD_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

//
// SMP2P bits a Qualcomm remote processor sets in its "slave-kernel" entry
// (the interrupts-extended of the remoteproc nodes in kodiak.dtsi).
//
#define SMP2P_BIT_FATAL     BIT0
#define SMP2P_BIT_READY     BIT1
#define SMP2P_BIT_HANDOVER  BIT2
#define SMP2P_BIT_STOP_ACK  BIT3

//
// RPMh resources a DSP needs voted while it boots (Linux's proxy votes).
//
#define DSP_MAX_RAILS  2
#define DSP_MAX_BCMS   3

typedef struct {
  CONST CHAR8    *Name;           // command DB name, e.g. "lcx.lvl"
  UINT32         FallbackAddr;    // address when the command DB is not usable
  UINT32         FallbackLevel;   // highest level then
} DSP_RAIL;

typedef struct {
  CONST CHAR8    *Name;           // command DB name, e.g. "CO0"
  UINT32         FallbackAddr;
  BOOLEAN        Commit;          // last BCM of its voter (VCD)
} DSP_BCM;

//
// What the SoC fixes about a DSP. The board's device tree gives the firmware
// file and the memory the DSP runs from.
//
typedef struct {
  CONST CHAR8    *Name;                  // QMP load_state and PIL relocation name
  CONST CHAR8    *Compatible;            // remoteproc node
  UINT32         PasId;                  // TrustZone peripheral ID
  UINT16         SmemHost;               // SMEM host ID of the DSP
  UINT32         Smp2pItem;              // SMEM item of its SMP2P entries to apps
  UINT32         Smp2pOutItem;           // SMEM item of apps' SMP2P entries to it
  UINT32         Smp2pIpcc;              // its SMP2P doorbell: IPCC client << 16 | signal
  UINT32         CrashReasonItem;        // SMEM item of its crash reason
  UINT16         StreamId;               // apps SMMU stream at EL2
  UINT16         StreamMask;
  DSP_RAIL       Rails[DSP_MAX_RAILS];
  DSP_BCM        Bcms[DSP_MAX_BCMS];     // Name NULL when unused
} DSP_DESC;

typedef enum {
  DspStateIdle,
  DspStateSkipped,
  DspStateFailed,
  DspStateStarted,        // authenticated and released from reset
  DspStateRunning,        // ready and handed over
} DSP_STATE;

typedef struct {
  UINT32     Addr;
  BOOLEAN    Voted;
} DSP_VOTE;

//
// A loaded firmware image, parsed as qcom_mdt_pas_init() and
// qcom_mdt_load_no_init() do.
//
typedef struct {
  UINT8                   *Data;
  UINTN                   Size;
  UINT8                   *Metadata;      // ELF and program headers, then the hash segment
  UINTN                   MetadataSize;
  BOOLEAN                 Relocatable;
  EFI_PHYSICAL_ADDRESS    MinAddr;        // lowest loadable segment
  EFI_PHYSICAL_ADDRESS    MaxAddr;        // end of the highest one, 4 KiB aligned
  CHAR8                   Version[96];    // QC_IMAGE_VERSION_STRING, if found
} DSP_IMAGE;

typedef struct {
  CONST DSP_DESC          *Desc;
  DSP_STATE               State;
  CONST CHAR8             *Reason;        // why it failed or was skipped
  EFI_STATUS              Status;
  CHAR8                   FirmwareName[128];
  EFI_PHYSICAL_ADDRESS    CarveoutBase;
  UINT64                  CarveoutSize;
  DSP_IMAGE               Image;
  DSP_VOTE                Rails[DSP_MAX_RAILS];
  DSP_VOTE                Bcms[DSP_MAX_BCMS];
  BOOLEAN                 LoadStateOn;
  EFI_PHYSICAL_ADDRESS    Metadata;       // the copy TrustZone was given
  UINTN                   MetadataPages;
  UINT64                  MetadataBridge;
  UINT64                  StartedAt;      // timer ticks at auth_and_reset
  UINT32                  Smp2pBits;
} DSP;

//
// Memory (DspPreloadDxe.c)
//

/**
  Sets the CPU's mapping attributes of a range, mapping it if it is not
  mapped yet: the carve-outs UEFI keeps out of its page tables.

  @param[in]  Base        The start, page aligned.
  @param[in]  Size        The size, page aligned.
  @param[in]  Attributes  EFI_MEMORY_* cacheability and permissions.

  @retval EFI_SUCCESS  Done.
  @retval Other        From the CPU architecture protocol.
**/
EFI_STATUS
DspSetMemoryAttributes (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size,
  IN UINT64                Attributes
  );

//
// SCM: secure monitor calls to TrustZone (Scm.c)
//

/**
  Asks TrustZone whether a peripheral ID can be loaded through PAS.

  @param[in]  PasId  The peripheral.

  @return  TRUE if TrustZone supports it, FALSE if not or if it cannot say.
**/
BOOLEAN
ScmPasIsSupported (
  IN UINT32  PasId
  );

/**
  Hands TrustZone the metadata (ELF headers and hash segment) of an image.

  @param[in]  PasId     The peripheral.
  @param[in]  Metadata  Physical address of the metadata: below 4 GiB, page
                        aligned, mapped uncached.

  @retval EFI_SUCCESS  Accepted.
  @retval Other        Refused; *TzStatus has why.
**/
EFI_STATUS
ScmPasInitImage (
  IN  UINT32                PasId,
  IN  EFI_PHYSICAL_ADDRESS  Metadata,
  OUT INT64                 *TzStatus
  );

/**
  Tells TrustZone where a relocatable image goes.

  @param[in]  PasId  The peripheral.
  @param[in]  Base   Physical address of the image.
  @param[in]  Size   Its size.

  @retval EFI_SUCCESS  Accepted.
  @retval Other        Refused; *TzStatus has why.
**/
EFI_STATUS
ScmPasMemSetup (
  IN  UINT32                PasId,
  IN  EFI_PHYSICAL_ADDRESS  Base,
  IN  UINT64                Size,
  OUT INT64                 *TzStatus
  );

/**
  Has TrustZone authenticate the loaded image and take the peripheral out of
  reset.

  @param[in]  PasId  The peripheral.

  @retval EFI_SUCCESS  The peripheral runs.
  @retval Other        Refused; *TzStatus has why.
**/
EFI_STATUS
ScmPasAuthAndReset (
  IN  UINT32  PasId,
  OUT INT64   *TzStatus
  );

/**
  Has TrustZone stop a peripheral and release its memory.

  @param[in]  PasId  The peripheral.

  @retval EFI_SUCCESS  Stopped.
  @retval Other        Refused.
**/
EFI_STATUS
ScmPasShutdown (
  IN UINT32  PasId
  );

/**
  Turns the SHM bridge on in TrustZone, after which it only accepts buffers
  of the non-secure world that lie in a bridge.

  @retval EFI_SUCCESS      On.
  @retval EFI_UNSUPPORTED  TrustZone has no SHM bridge.
  @retval Other            Refused.
**/
EFI_STATUS
ScmShmBridgeEnable (
  VOID
  );

/**
  Creates an SHM bridge over a buffer, owned by the non-secure world.

  @param[in]   Base    Physical address, page aligned.
  @param[in]   Size    Size, page aligned.
  @param[out]  Handle  The bridge.

  @retval EFI_SUCCESS  Created.
  @retval Other        Refused.
**/
EFI_STATUS
ScmShmBridgeCreate (
  IN  EFI_PHYSICAL_ADDRESS  Base,
  IN  UINT64                Size,
  OUT UINT64                *Handle
  );

/**
  Deletes an SHM bridge.

  @param[in]  Handle  The bridge.
**/
VOID
ScmShmBridgeDelete (
  IN UINT64  Handle
  );

//
// MDT/MBN images (Mdt.c)
//

/**
  Checks a firmware image the way qcom_mdt_pas_init() does and builds its
  metadata. Only images that hold all their segments (.mbn) are supported.

  @param[in,out]  Image  Data and Size set; the rest is filled in.

  @retval EFI_SUCCESS            Usable.
  @retval EFI_UNSUPPORTED        Split image (.mdt with .bNN files).
  @retval EFI_VOLUME_CORRUPTED   Not a valid image.
  @retval EFI_OUT_OF_RESOURCES   No memory for the metadata.
**/
EFI_STATUS
MdtParse (
  IN OUT DSP_IMAGE  *Image
  );

/**
  Copies the loadable segments into the memory the image runs from and
  clears what they do not fill, as qcom_mdt_load_no_init() does.

  @param[in]  Image   The parsed image.
  @param[in]  Region  The memory, mapped.
  @param[in]  Base    Its physical address.
  @param[in]  Size    Its size.

  @retval EFI_SUCCESS            Loaded.
  @retval EFI_VOLUME_CORRUPTED   A segment does not fit.
**/
EFI_STATUS
MdtLoadSegments (
  IN CONST DSP_IMAGE       *Image,
  IN VOID                  *Region,
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  );

/**
  Frees what MdtParse() allocated and the image data.

  @param[in,out]  Image  The image.
**/
VOID
MdtFree (
  IN OUT DSP_IMAGE  *Image
  );

//
// SMEM and SMP2P (Smem.c)
//

/**
  Maps SMEM for reading by the CPU.

  @retval EFI_SUCCESS  SMEM can be read.
  @retval Other        It cannot.
**/
EFI_STATUS
SmemInit (
  VOID
  );

/**
  Reads the SMP2P entry a remote processor sets for apps ("slave-kernel").

  @param[in]   RemoteHost  The remote processor's SMEM host ID.
  @param[in]   Item        The SMEM item of its SMP2P entries to apps.
  @param[out]  Bits        The entry's value.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  The remote has not created the item.
  @retval EFI_NOT_READY  It has, but not the entry yet.
  @retval Other          SMEM is not usable.
**/
EFI_STATUS
SmemReadSmp2pInbound (
  IN  UINT16  RemoteHost,
  IN  UINT32  Item,
  OUT UINT32  *Bits
  );

/**
  Clears the SMP2P entry a remote processor sets for apps, if there is one,
  so that what is read from it afterwards is what the remote processor set
  since. Only for a remote processor held in reset.

  @param[in]  RemoteHost  The remote processor's SMEM host ID.
  @param[in]  Item        The SMEM item of its SMP2P entries to apps.

  @retval EFI_SUCCESS    Cleared.
  @retval EFI_NOT_FOUND  There is no such entry.
  @retval Other          SMEM is not usable.
**/
EFI_STATUS
SmemClearSmp2pInbound (
  IN UINT16  RemoteHost,
  IN UINT32  Item
  );

/**
  Creates the SMP2P entries apps sets for a remote processor
  ("master-kernel", all clear) as Linux's SMP2P driver does when it starts,
  and rings the remote's SMP2P doorbell. A DSP may wait for them.

  @param[in]  RemoteHost   The remote processor's SMEM host ID.
  @param[in]  Item         The SMEM item of apps' SMP2P entries to it.
  @param[in]  InboundItem  The SMEM item of its SMP2P entries to apps.
  @param[in]  IpccSignal   Its SMP2P doorbell: IPCC client << 16 | signal.

  @retval EFI_SUCCESS  Created, or set up again.
  @retval Other        SMEM is not usable.
**/
EFI_STATUS
SmemCreateSmp2pOutbound (
  IN UINT16  RemoteHost,
  IN UINT32  Item,
  IN UINT32  InboundItem,
  IN UINT32  IpccSignal
  );

/**
  Negotiates the SMP2P version with a remote processor, as Linux's SMP2P
  driver does when the remote signals it: a remote does not add its entries
  until both sides have the same version.

  @param[in]  RemoteHost   The remote processor's SMEM host ID.
  @param[in]  Item         The SMEM item of apps' SMP2P entries to it.
  @param[in]  InboundItem  The SMEM item of its SMP2P entries to apps.
  @param[in]  IpccSignal   Its SMP2P doorbell.

  @retval EFI_SUCCESS    Both sides have the same version.
  @retval EFI_NOT_READY  Not yet: the remote's item is not there, or apps
                         just came down to its version.
**/
EFI_STATUS
SmemNegotiateSmp2p (
  IN UINT16  RemoteHost,
  IN UINT32  Item,
  IN UINT32  InboundItem,
  IN UINT32  IpccSignal
  );

/**
  Logs the items of the SMEM partition apps shares with a remote host, and
  the SMP2P entries among them.

  @param[in]  RemoteHost  The remote host.
**/
VOID
SmemLogPartition (
  IN UINT16  RemoteHost
  );

/**
  Finds an item in the global SMEM partition, or in the global heap of an
  SMEM without partitions.

  @param[in]   Item  The item.
  @param[out]  Data  Its data.
  @param[out]  Size  Its size.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  Not there.
**/
EFI_STATUS
SmemGetGlobalItem (
  IN  UINT32  Item,
  OUT VOID    **Data,
  OUT UINTN   *Size
  );

//
// AOSS QMP: messages to AOP (Qmp.c)
//

/**
  Sends AOP the load state of a DSP, opening the QMP link first if needed.

  @param[in]  Name  The DSP ("adsp", "cdsp").
  @param[in]  On    TRUE for "on", FALSE for "off".

  @retval EFI_SUCCESS  AOP took the message.
  @retval Other        The link could not be opened or AOP did not answer.
**/
EFI_STATUS
QmpSendLoadState (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      On
  );

//
// Command DB (CmdDb.c)
//

/**
  Maps the AOP command DB for reading.

  @param[in]  Base  Its physical address.
  @param[in]  Size  Its size.

  @retval EFI_SUCCESS  The command DB can be read.
  @retval Other        It cannot, or holds no valid header.
**/
EFI_STATUS
CmdDbInit (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  );

/**
  Looks up an RPMh resource in the command DB.

  @param[in]   Name     The resource, at most 8 characters.
  @param[out]  Addr     Its RPMh address.
  @param[out]  AuxData  Its auxiliary data, OPTIONAL.
  @param[out]  AuxSize  Its size, OPTIONAL.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  Not there, or the command DB is not usable.
**/
EFI_STATUS
CmdDbLookup (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Addr,
  OUT CONST UINT8  **AuxData OPTIONAL,
  OUT UINTN        *AuxSize OPTIONAL
  );

/**
  Returns the highest level of a power rail (ARC) resource, from the level
  table in its auxiliary data, as rpmhpd does.

  @param[in]   Name      The resource, e.g. "cx.lvl".
  @param[out]  Addr      Its RPMh address.
  @param[out]  MaxLevel  Its highest level (hardware level index).

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  Not there, or no level table.
**/
EFI_STATUS
CmdDbRailMaxLevel (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Addr,
  OUT UINT32       *MaxLevel
  );

//
// Firmware files (Firmware.c)
//

/**
  Reads a firmware file the way Linux's firmware loader finds it, from the
  file systems of the partitions named PcdDspFirmwarePartition:
  usr/lib/firmware/updates/<Name>, then usr/lib/firmware/<Name>.

  @param[in]   Name  The firmware name, as in the device tree.
  @param[out]  Data  The file, from the pool.
  @param[out]  Size  Its size.

  @retval EFI_SUCCESS    Read.
  @retval EFI_NOT_FOUND  No such file.
  @retval Other          It could not be read.
**/
EFI_STATUS
FirmwareRead (
  IN  CONST CHAR8  *Name,
  OUT UINT8        **Data,
  OUT UINTN        *Size
  );

//
// Device tree (DeviceTree.c)
//

/**
  Reads what the board's device tree says about a DSP: whether it is
  enabled, its firmware name and the memory it runs from.

  @param[in,out]  Dsp  Desc set; FirmwareName and Carveout* are filled in.

  @retval EFI_SUCCESS    Found and enabled.
  @retval EFI_NOT_FOUND  No device tree, no such node, or it is disabled.
  @retval Other          The node is incomplete.
**/
EFI_STATUS
DtGetDsp (
  IN OUT DSP  *Dsp
  );

/**
  Finds the AOP command DB in the board's device tree.

  @param[out]  Base  Its physical address.
  @param[out]  Size  Its size.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  Not there.
**/
EFI_STATUS
DtGetCmdDb (
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINT64                *Size
  );

/**
  Removes the iommus property from the remoteproc nodes of the DSPs that run,
  so that Linux leaves their SMMU streams in the bypass UEFI handed over.

  @param[in]  Fdt   The device tree.
  @param[in]  Dsps  The DSPs.
  @param[in]  Count How many.

  @return  The number of properties removed.
**/
UINTN
DtFixupRunningDsps (
  IN VOID       *Fdt,
  IN CONST DSP  *Dsps,
  IN UINTN      Count
  );

// Populate the board's simple-framebuffer template before an OS loader runs.
VOID
DtPrepareDisplay (
  VOID
  );

/**
  Installs EFI_DT_FIXUP_PROTOCOL, through which GRUB has the device tree it
  loads fixed up for the DSPs that run.

  @param[in]  Dsps   The DSPs.
  @param[in]  Count  How many.

  @retval EFI_SUCCESS  Installed.
  @retval Other        It could not be.
**/
EFI_STATUS
DtInstallFixupProtocol (
  IN CONST DSP  *Dsps,
  IN UINTN      Count
  );

#endif // DSP_PRELOAD_H_
