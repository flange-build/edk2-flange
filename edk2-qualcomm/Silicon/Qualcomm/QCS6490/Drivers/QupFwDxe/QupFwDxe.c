/** @file
  Loads the QUP serial engine firmware for the OS.

  The GENI serial engines of the QCS6490 QUP wrappers run protocol firmware
  (I2C, SPI, UART) that has to be loaded into each engine before use. XBL only
  loads it for the engines it uses itself, such as the debug UART. The stock
  Qualcomm UEFI loads the others, and the vendor kernels rely on that: without
  it, every I2C, SPI and UART controller of the board fails to probe, the
  HDMI bridge behind I2C9 among them.

  Once UFS brings up the qupfw_a partition, which holds the Qualcomm
  qupv3fw.elf image, load the firmware for the protocol each engine listed in
  PcdQupFwSerialEngines has in the board's device tree, in FIFO mode. Engines
  that already run firmware are left alone.

  The loading sequence follows the Linux GENI serial engine driver
  (drivers/soc/qcom/qcom-geni-se.c).

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>

//
// GPT partition type of qupfw_a/qupfw_b.
//
STATIC CONST EFI_GUID  mQupFwPartitionType = {
  0x21d1219f, 0x2ed1, 0x4ab4, { 0x93, 0x0a, 0x41, 0xa1, 0x6a, 0xe7, 0x5f, 0x7f }
};

#define QUP_FW_PARTITION_NAME  L"qupfw_a"

//
// PcdQupFwSerialEngines entries: serial engine base | protocol.
//
#define SE_ENTRY_BASE_MASK      0xFFFFC000
#define SE_ENTRY_PROTOCOL_MASK  0x000000FF

#define GENI_SE_SPI      1
#define GENI_SE_UART     2
#define GENI_SE_I2C      3
#define GENI_SE_INVALID  0xFF

//
// QUP wrappers: serial engine k of wrapper w is at SeBase + k * 0x4000.
//
#define QUP_SE_SIZE    0x4000
#define QUP_SE_COUNT   8
#define QUP0_SE_BASE   0x00980000
#define QUP0_BASE      0x009C0000
#define QUP1_SE_BASE   0x00A80000
#define QUP1_BASE      0x00AC0000

//
// GCC (gcc-sc7280.c).
//
#define GCC_BASE                          0x00100000
#define GCC_APCS_CLOCK_BRANCH_ENA_VOTE    (GCC_BASE + 0x52000)
#define GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1  (GCC_BASE + 0x52008)
#define GCC_QUP0_SE_CBCR(k)               (GCC_BASE + 0x1700C + (k) * 0x130)
#define GCC_QUP1_SE_CBCR(k)               (GCC_BASE + 0x1800C + (k) * 0x130)
#define GCC_CBCR_CLK_OFF                  BIT31
#define GCC_RCGR_CMD_UPDATE               BIT0
#define GCC_RCGR_CMD_ROOT_OFF             BIT31

//
// QUP wrapper registers.
//
#define QUPV3_HW_VER           0x004
#define QUPV3_SE_AHB_M_CFG     0x118
#define QUPV3_COMMON_CFG       0x120
#define QUPV3_COMMON_CGC_CTRL  0x21C

//
// Serial engine registers.
//
#define SE_GENI_INIT_CFG_REVISION     0x000
#define SE_GENI_S_INIT_CFG_REVISION   0x004
#define GENI_FORCE_DEFAULT_REG        0x020
#define GENI_OUTPUT_CTRL              0x024
#define SE_GENI_CGC_CTRL              0x028
#define GENI_IF_DISABLE_RO            0x064
#define GENI_FW_REVISION_RO           0x068
#define SE_GENI_CFG_REG0              0x100
#define SE_GENI_DMA_MODE_EN           0x258
#define SE_GENI_M_IRQ_EN              0x614
#define SE_GENI_S_IRQ_ENABLE          0x644
#define SE_GENI_RX_RFR_WATERMARK_REG  0x814
#define SE_DMA_TX_IRQ_EN_SET          0xC4C
#define SE_DMA_RX_IRQ_EN_SET          0xD4C
#define SE_GSI_EVENT_EN               0xE18
#define SE_IRQ_EN                     0xE1C
#define SE_HW_PARAM_1                 0xE28
#define SE_DMA_GENERAL_CFG            0xE30
#define SE_GENI_FW_REVISION           0x1000
#define SE_GENI_S_FW_REVISION         0x1004
#define SE_GENI_CFG_RAMN              0x1010
#define SE_GENI_CLK_CTRL              0x2000
#define SE_DMA_IF_EN                  0x2004
#define SE_FIFO_IF_DISABLE            0x2008

#define PROG_RAM_HCLK_OFF   BIT8
#define PROG_RAM_SCLK_OFF   BIT9
#define DEFAULT_CGC_EN      0x7F
#define DEFAULT_IO_OUTPUT   0x7F
#define DMA_CGC_ON          0xF
#define SER_CLK_SEL         BIT0
#define DMA_IF_EN           BIT0
#define FIFO_IF_DISABLE     BIT0

//
// IRQ enables the Linux loader sets for FIFO mode.
//
#define SE_IRQ_EN_ALL          0xF
#define M_COMMON_GENI_M_IRQ    0x33C0007E
#define S_COMMON_GENI_S_IRQ    0x03001E36
#define DMA_TX_IRQ_COMMON      0x0D
#define DMA_RX_IRQ_COMMON      0x1D

#define MAX_GENI_CFG_RAMN_CNT  455

//
// qupv3fw.elf: a 32-bit ELF with one segment per protocol image.
//
#define SE_FW_MAGIC  0x57464553   // "SEFW"

#pragma pack(1)
typedef struct {
  UINT32    Magic;
  UINT32    Version;
  UINT32    CoreVersion;
  UINT16    SerialProtocol;
  UINT16    FwVersion;
  UINT16    CfgVersion;
  UINT16    FwSizeInItems;
  UINT16    FwOffset;
  UINT16    CfgSizeInItems;
  UINT16    CfgIdxOffset;
  UINT16    CfgValOffset;
} SE_FW_HEADER;

typedef struct {
  UINT8     Ident[16];
  UINT16    Type;
  UINT16    Machine;
  UINT32    Version;
  UINT32    Entry;
  UINT32    PhOff;
  UINT32    ShOff;
  UINT32    Flags;
  UINT16    EhSize;
  UINT16    PhEntSize;
  UINT16    PhNum;
  UINT16    ShEntSize;
  UINT16    ShNum;
  UINT16    ShStrNdx;
} ELF32_HEADER;

typedef struct {
  UINT32    Type;
  UINT32    Offset;
  UINT32    VAddr;
  UINT32    PAddr;
  UINT32    FileSz;
  UINT32    MemSz;
  UINT32    Flags;
  UINT32    Align;
} ELF32_PHDR;
#pragma pack()

#define ELF_PT_LOAD  1

//
// Qualcomm segment flags.
//
#define MI_PBT_PAGE_MODE(f)      (((f) >> 20) & 0x1)
#define MI_PBT_ACCESS_TYPE(f)    (((f) >> 21) & 0x7)
#define MI_PBT_SEGMENT_TYPE(f)   (((f) >> 24) & 0x7)
#define MI_PBT_HASH_SEGMENT      2
#define MI_PBT_NOTUSED_SEGMENT   3
#define MI_PBT_SHARED_SEGMENT    4

STATIC EFI_EVENT  mBlockIoEvent;
STATIC VOID       *mBlockIoRegistration;
STATIC BOOLEAN    mLoaded;

STATIC CONST CHAR8  *mProtocolNames[] = { "none", "SPI", "UART", "I2C" };

/**
  Returns the name of a protocol, for the log.
**/
STATIC
CONST CHAR8 *
ProtocolName (
  IN UINTN  Protocol
  )
{
  return (Protocol < ARRAY_SIZE (mProtocolNames)) ? mProtocolNames[Protocol] : "?";
}

/**
  Finds the firmware image for a protocol in qupv3fw.elf.

  @param[in]  Image      The ELF image.
  @param[in]  ImageSize  Its size.
  @param[in]  Protocol   The protocol.

  @return  The image header, or NULL.
**/
STATIC
SE_FW_HEADER *
FindProtocolFirmware (
  IN UINT8  *Image,
  IN UINTN  ImageSize,
  IN UINTN  Protocol
  )
{
  ELF32_HEADER  *Ehdr;
  ELF32_PHDR    *Phdr;
  SE_FW_HEADER  *Header;
  UINTN         Index;
  UINTN         FwSize;

  Ehdr = (ELF32_HEADER *)Image;
  if ((ImageSize < sizeof (*Ehdr)) ||
      (CompareMem (Ehdr->Ident, "\x7F" "ELF", 4) != 0) ||
      (Ehdr->Ident[4] != 1) ||
      (Ehdr->PhEntSize != sizeof (ELF32_PHDR)) ||
      ((UINT64)Ehdr->PhOff + (UINT64)Ehdr->PhNum * sizeof (ELF32_PHDR) > ImageSize))
  {
    return NULL;
  }

  for (Index = 0; Index < Ehdr->PhNum; Index++) {
    Phdr = (ELF32_PHDR *)(Image + Ehdr->PhOff) + Index;

    if ((Phdr->Type != ELF_PT_LOAD) || (Phdr->MemSz == 0) ||
        ((UINT64)Phdr->Offset + Phdr->FileSz > ImageSize) ||
        (Phdr->FileSz < sizeof (SE_FW_HEADER)) ||
        (MI_PBT_PAGE_MODE (Phdr->Flags) != 0) ||
        (MI_PBT_SEGMENT_TYPE (Phdr->Flags) == MI_PBT_HASH_SEGMENT) ||
        (MI_PBT_ACCESS_TYPE (Phdr->Flags) == MI_PBT_NOTUSED_SEGMENT) ||
        (MI_PBT_ACCESS_TYPE (Phdr->Flags) == MI_PBT_SHARED_SEGMENT))
    {
      continue;
    }

    Header = (SE_FW_HEADER *)(Image + Phdr->Offset);
    if ((Header->Magic != SE_FW_MAGIC) || (Header->Version != 1) ||
        (Header->SerialProtocol != Protocol))
    {
      continue;
    }

    //
    // The RAM is written in pairs of words.
    //
    FwSize = ALIGN_VALUE (Header->FwSizeInItems, 2);
    if ((FwSize >= MAX_GENI_CFG_RAMN_CNT) ||
        (Header->FwOffset + Header->FwSizeInItems * sizeof (UINT32) > Phdr->FileSz) ||
        (Header->CfgIdxOffset + Header->CfgSizeInItems * sizeof (UINT8) > Phdr->FileSz) ||
        (Header->CfgValOffset + Header->CfgSizeInItems * sizeof (UINT32) > Phdr->FileSz))
    {
      DEBUG ((DEBUG_ERROR, "QupFw: corrupt %a image in segment %u\n", ProtocolName (Protocol), Index));
      continue;
    }

    return Header;
  }

  return NULL;
}

/**
  Turns on the clocks of a serial engine and of its wrapper.

  @param[in]  SeBase  The serial engine.

  @retval EFI_SUCCESS  The clocks run.
  @retval Other        They do not; the engine must not be touched.
**/
STATIC
EFI_STATUS
EnableSeClocks (
  IN UINTN  SeBase
  )
{
  UINTN   Index;
  UINTN   Cbcr;
  UINTN   Rcg;
  UINTN   Timeout;
  UINT32  WrapperVote;

  if ((SeBase >= QUP0_SE_BASE) && (SeBase < QUP0_SE_BASE + QUP_SE_COUNT * QUP_SE_SIZE)) {
    Index = (SeBase - QUP0_SE_BASE) / QUP_SE_SIZE;
    //
    // m_ahb, s_ahb, core, core_2x, and the engine.
    //
    WrapperVote = BIT6 | BIT7 | BIT8 | BIT9;
    MmioOr32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, WrapperVote | (BIT10 << Index));
    Cbcr = GCC_QUP0_SE_CBCR (Index);
  } else if ((SeBase >= QUP1_SE_BASE) && (SeBase < QUP1_SE_BASE + QUP_SE_COUNT * QUP_SE_SIZE)) {
    Index = (SeBase - QUP1_SE_BASE) / QUP_SE_SIZE;
    //
    // core_2x, core, m_ahb, s_ahb, and the engine.
    //
    WrapperVote = BIT18 | BIT19 | BIT20 | BIT21;
    if (Index < 6) {
      MmioOr32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, WrapperVote | (BIT22 << Index));
    } else {
      MmioOr32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, WrapperVote);
      MmioOr32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE, BIT13 << (Index - 6));
    }

    Cbcr = GCC_QUP1_SE_CBCR (Index);
  } else {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The root clock generator sits right behind the branch. Should its
  // source be off, run it from the 19.2 MHz crystal instead.
  //
  Rcg = Cbcr + 4;
  for (Timeout = 0; Timeout < 100; Timeout++) {
    if ((MmioRead32 (Cbcr) & GCC_CBCR_CLK_OFF) == 0) {
      return EFI_SUCCESS;
    }

    if ((Timeout == 50) && ((MmioRead32 (Rcg) & GCC_RCGR_CMD_ROOT_OFF) != 0)) {
      DEBUG ((DEBUG_INFO, "QupFw: 0x%x: clock source off, using XO\n", SeBase));
      MmioWrite32 (Rcg + 4, 0);
      MmioOr32 (Rcg, GCC_RCGR_CMD_UPDATE);
    }

    MicroSecondDelay (10);
  }

  DEBUG ((DEBUG_ERROR, "QupFw: 0x%x: clock does not turn on (CBCR 0x%x)\n", SeBase, MmioRead32 (Cbcr)));
  return EFI_DEVICE_ERROR;
}

/**
  Loads protocol firmware into a serial engine, in FIFO mode.

  @param[in]  SeBase     The serial engine.
  @param[in]  Header     The firmware image.

  @retval EFI_SUCCESS  The engine runs the firmware.
  @retval Other        It does not.
**/
STATIC
EFI_STATUS
LoadSeFirmware (
  IN UINTN         SeBase,
  IN SE_FW_HEADER  *Header
  )
{
  UINTN         WrapperBase;
  CONST UINT32  *FwData;
  CONST UINT8   *CfgIdx;
  CONST UINT32  *CfgVal;
  UINTN         Index;
  UINTN         FwSize;
  UINT32        HwVersion;
  UINT32        Depth;
  UINT32        Revision;

  WrapperBase = (SeBase < QUP1_SE_BASE) ? QUP0_BASE : QUP1_BASE;
  FwData      = (CONST UINT32 *)((UINT8 *)Header + Header->FwOffset);
  CfgIdx      = (CONST UINT8 *)Header + Header->CfgIdxOffset;
  CfgVal      = (CONST UINT32 *)((UINT8 *)Header + Header->CfgValOffset);
  Revision    = ((UINT32)Header->SerialProtocol << 8) | (Header->FwVersion & 0xFF);

  //
  // Wrapper: no fast switch to high priority interrupts, and hardware
  // controlled clock gating.
  //
  MmioOr32 (WrapperBase + QUPV3_COMMON_CFG, BIT0);
  MmioOr32 (WrapperBase + QUPV3_SE_AHB_M_CFG, BIT0);
  MmioOr32 (WrapperBase + QUPV3_COMMON_CGC_CTRL, BIT0);

  MmioWrite32 (SeBase + GENI_OUTPUT_CTRL, 0);

  //
  // Clocks for programming the RAM.
  //
  MmioOr32 (SeBase + SE_GENI_CGC_CTRL, PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF);
  MmioWrite32 (SeBase + SE_GENI_CLK_CTRL, 0);
  MmioAnd32 (SeBase + SE_GENI_CGC_CTRL, ~(UINT32)(PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF));
  MmioOr32 (SeBase + SE_DMA_GENERAL_CFG, DMA_CGC_ON);
  MmioWrite32 (SeBase + SE_GENI_CGC_CTRL, DEFAULT_CGC_EN);

  //
  // Configuration: version and primitive table.
  //
  MmioWrite32 (SeBase + SE_GENI_INIT_CFG_REVISION, Header->CfgVersion);
  MmioWrite32 (SeBase + SE_GENI_S_INIT_CFG_REVISION, Header->CfgVersion);
  for (Index = 0; Index < Header->CfgSizeInItems; Index++) {
    MmioWrite32 (SeBase + SE_GENI_CFG_REG0 + CfgIdx[Index] * sizeof (UINT32), CfgVal[Index]);
  }

  HwVersion = MmioRead32 (WrapperBase + QUPV3_HW_VER);
  if (((HwVersion >> 28) > 3) || (((HwVersion >> 28) == 3) && (((HwVersion >> 16) & 0xFFF) >= 10))) {
    Depth = (MmioRead32 (SeBase + SE_HW_PARAM_1) >> 16) & 0xFF;
  } else {
    Depth = (MmioRead32 (SeBase + SE_HW_PARAM_1) >> 16) & 0x3F;
  }

  MmioWrite32 (SeBase + SE_GENI_RX_RFR_WATERMARK_REG, Depth - 2);
  MmioOr32 (SeBase + GENI_OUTPUT_CTRL, DEFAULT_IO_OUTPUT);

  //
  // FIFO mode.
  //
  MmioAnd32 (SeBase + SE_GENI_DMA_MODE_EN, ~(UINT32)BIT0);
  MmioWrite32 (SeBase + SE_IRQ_EN, SE_IRQ_EN_ALL);
  MmioWrite32 (SeBase + SE_GSI_EVENT_EN, 0);

  MmioWrite32 (SeBase + SE_GENI_M_IRQ_EN, M_COMMON_GENI_M_IRQ);
  MmioWrite32 (SeBase + SE_GENI_S_IRQ_ENABLE, S_COMMON_GENI_S_IRQ);
  MmioWrite32 (SeBase + SE_DMA_TX_IRQ_EN_SET, DMA_TX_IRQ_COMMON);
  MmioWrite32 (SeBase + SE_DMA_RX_IRQ_EN_SET, DMA_RX_IRQ_COMMON);

  MmioWrite32 (SeBase + SE_GENI_FW_REVISION, Revision);
  MmioWrite32 (SeBase + SE_GENI_S_FW_REVISION, Revision);

  //
  // The program RAM, in pairs of words.
  //
  FwSize = ALIGN_VALUE (Header->FwSizeInItems, 2);
  for (Index = 0; Index < FwSize; Index++) {
    MmioWrite32 (
      SeBase + SE_GENI_CFG_RAMN + Index * sizeof (UINT32),
      (Index < Header->FwSizeInItems) ? FwData[Index] : 0
      );
  }

  MmioWrite32 (SeBase + GENI_FORCE_DEFAULT_REG, BIT0);

  //
  // Apply: toggle the RAM clocks, and select the serial clock.
  //
  MmioOr32 (SeBase + SE_GENI_CGC_CTRL, PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF);
  MmioOr32 (SeBase + SE_GENI_CLK_CTRL, SER_CLK_SEL);
  MmioAnd32 (SeBase + SE_GENI_CGC_CTRL, ~(UINT32)(PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF));

  MmioOr32 (SeBase + SE_DMA_IF_EN, DMA_IF_EN);
  MmioAnd32 (SeBase + SE_FIFO_IF_DISABLE, ~(UINT32)FIFO_IF_DISABLE);

  if (((MmioRead32 (SeBase + GENI_FW_REVISION_RO) & 0xFFFF) != Revision) ||
      ((MmioRead32 (SeBase + GENI_IF_DISABLE_RO) & FIFO_IF_DISABLE) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Loads the firmware of every engine the board lists.

  @param[in]  Image      qupv3fw.elf.
  @param[in]  ImageSize  Its size.
**/
STATIC
VOID
LoadAllSeFirmware (
  IN UINT8  *Image,
  IN UINTN  ImageSize
  )
{
  CONST UINT32  *Entries;
  UINTN         Count;
  UINTN         Index;
  UINTN         SeBase;
  UINTN         Protocol;
  UINTN         Current;
  SE_FW_HEADER  *Header;
  EFI_STATUS    Status;

  Entries = PcdGetPtr (PcdQupFwSerialEngines);
  Count   = PcdGetSize (PcdQupFwSerialEngines) / sizeof (UINT32);

  for (Index = 0; Index < Count && Entries[Index] != 0; Index++) {
    SeBase   = Entries[Index] & SE_ENTRY_BASE_MASK;
    Protocol = Entries[Index] & SE_ENTRY_PROTOCOL_MASK;

    Status = EnableSeClocks (SeBase);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Current = (MmioRead32 (SeBase + GENI_FW_REVISION_RO) >> 8) & 0xFF;
    if ((Current != 0) && (Current != GENI_SE_INVALID)) {
      DEBUG ((
        DEBUG_INFO,
        "QupFw: 0x%x already runs %a firmware\n",
        SeBase,
        ProtocolName (Current)
        ));
      continue;
    }

    Header = FindProtocolFirmware (Image, ImageSize, Protocol);
    if (Header == NULL) {
      DEBUG ((DEBUG_ERROR, "QupFw: no %a firmware in qupv3fw.elf\n", ProtocolName (Protocol)));
      continue;
    }

    Status = LoadSeFirmware (SeBase, Header);
    DEBUG ((
      EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
      "QupFw: 0x%x: %a firmware 0x%x: %r\n",
      SeBase,
      ProtocolName (Protocol),
      Header->FwVersion,
      Status
      ));
  }
}

/**
  Looks for qupfw_a among newly installed block devices.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
OnBlockIo (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   Handle;
  UINTN                        HandleSize;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  UINTN                        Size;
  UINTN                        Pages;
  UINT8                        *Image;

  while (!mLoaded) {
    HandleSize = sizeof (Handle);
    Status     = gBS->LocateHandle (
                        ByRegisterNotify,
                        NULL,
                        mBlockIoRegistration,
                        &HandleSize,
                        &Handle
                        );
    if (EFI_ERROR (Status)) {
      return;
    }

    Status = gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&PartitionInfo);
    if (EFI_ERROR (Status) ||
        (PartitionInfo->Type != PARTITION_TYPE_GPT) ||
        !CompareGuid (&PartitionInfo->Info.Gpt.PartitionTypeGUID, &mQupFwPartitionType) ||
        (StrnCmp (PartitionInfo->Info.Gpt.PartitionName, QUP_FW_PARTITION_NAME, ARRAY_SIZE (PartitionInfo->Info.Gpt.PartitionName)) != 0))
    {
      continue;
    }

    Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Size  = (UINTN)(BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
    Pages = EFI_SIZE_TO_PAGES (Size);
    Image = AllocatePages (Pages);
    if (Image == NULL) {
      return;
    }

    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, Size, Image);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "QupFw: reading %s: %r\n", QUP_FW_PARTITION_NAME, Status));
    } else {
      DEBUG ((DEBUG_INFO, "QupFw: %s: %u bytes\n", QUP_FW_PARTITION_NAME, Size));
      LoadAllSeFirmware (Image, Size);
      mLoaded = TRUE;
    }

    FreePages (Image, Pages);
  }

  gBS->CloseEvent (Event);
}

/**
  Warns if the firmware never got loaded.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!mLoaded) {
    DEBUG ((
      DEBUG_WARN,
      "QupFw: no %s partition found, the OS will miss the I2C/SPI/UART firmware\n",
      QUP_FW_PARTITION_NAME
      ));
  }

  gBS->CloseEvent (Event);
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The partition is being watched for.
  @retval Other        It could not be.
**/
EFI_STATUS
EFIAPI
QupFwDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ReadyToBootEvent;

  mBlockIoEvent = EfiCreateProtocolNotifyEvent (
                    &gEfiBlockIoProtocolGuid,
                    TPL_CALLBACK,
                    OnBlockIo,
                    NULL,
                    &mBlockIoRegistration
                    );
  if (mBlockIoEvent == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &ReadyToBootEvent);
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
