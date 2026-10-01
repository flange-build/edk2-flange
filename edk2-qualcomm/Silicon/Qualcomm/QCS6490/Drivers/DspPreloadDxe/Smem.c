/** @file
  Reading Qualcomm shared memory (SMEM) and SMP2P state for the DSP preload,
  the way Linux's drivers/soc/qcom/smem.c and smp2p.c find them.

  SMEM is divided into partitions, each shared by two hosts, listed in the
  partition table in its last 4 KiB. Items in a partition are found by
  walking its uncached list up from the partition header, then its cached
  list down from its end. A remote processor's SMP2P entries for apps are an
  item in the partition apps shares with it, and so are apps' entries for
  it, which Linux's SMP2P driver creates when it starts and a DSP looks for.
  Allocating an item takes the SMEM hardware lock, as Linux does.

  The CPU reads SMEM through a non-cacheable mapping, as Linux maps it, so
  that it sees what the DSPs write.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/PcdLib.h>

#include "DspPreload.h"

#define SMEM_PTABLE_MAGIC    SIGNATURE_32 ('$', 'T', 'O', 'C')
#define SMEM_PART_MAGIC      SIGNATURE_32 ('$', 'P', 'R', 'T')
#define SMEM_PRIVATE_CANARY  0xA5A5
#define SMEM_HOST_APPS       0
#define SMEM_GLOBAL_HOST     0xFFFE

//
// The SBL's entry of the version array in the SMEM header says how global
// items are kept.
//
#define SMEM_MASTER_SBL_VERSION_INDEX  7
#define SMEM_GLOBAL_PART_VERSION       12

#define SMP2P_MAGIC            SIGNATURE_32 ('$', 'S', 'M', 'P')
#define SMP2P_MAX_ENTRY        16
#define SMP2P_MAX_ENTRY_NAME   16
#define SMP2P_MAX_VERSION      2
#define SMP2P_FEATURE_SSR_ACK  BIT0
#define SMP2P_INBOUND_ENTRY    "slave-kernel"
#define SMP2P_OUTBOUND_ENTRY   "master-kernel"

//
// The SMEM hardware lock: TCSR mutex 3 (kodiak.dtsi smem hwlocks,
// tcsr_mutex at 0x1f40000 with a stride of 0x1000). A host takes it by
// writing its ID and reading it back (qcom_hwspinlock.c).
//
#define SMEM_HWLOCK_ADDR     (0x01F40000 + 3 * 0x1000)
#define SMEM_HWLOCK_APPS     1
#define SMEM_HWLOCK_TIMEOUT  1000000

//
// IPCC doorbell (qcom-ipcc.c).
//
#define IPCC_SEND_ID  (0x00408000 + 0x0C)

#pragma pack(1)

typedef struct {
  UINT32    Command;
  UINT32    Status;
  UINT32    Params[2];
} SMEM_PROC_COMM;

typedef struct {
  UINT32    Allocated;
  UINT32    Offset;
  UINT32    Size;
  UINT32    AuxBase;
} SMEM_GLOBAL_ENTRY;

typedef struct {
  SMEM_PROC_COMM       ProcComm[4];
  UINT32               Version[32];
  UINT32               Initialized;
  UINT32               FreeOffset;
  UINT32               Available;
  UINT32               Reserved;
  SMEM_GLOBAL_ENTRY    Toc[];
} SMEM_HEADER;

typedef struct {
  UINT32    Offset;
  UINT32    Size;
  UINT32    Flags;
  UINT16    Host0;
  UINT16    Host1;
  UINT32    Cacheline;
  UINT32    Reserved[7];
} SMEM_PTABLE_ENTRY;

typedef struct {
  UINT32               Magic;
  UINT32               Version;
  UINT32               NumEntries;
  UINT32               Reserved[5];
  SMEM_PTABLE_ENTRY    Entry[];
} SMEM_PTABLE;

typedef struct {
  UINT32    Magic;
  UINT16    Host0;
  UINT16    Host1;
  UINT32    Size;
  UINT32    OffsetFreeUncached;
  UINT32    OffsetFreeCached;
  UINT32    Reserved[3];
} SMEM_PARTITION_HEADER;

typedef struct {
  UINT16    Canary;
  UINT16    Item;
  UINT32    Size;           // padding included
  UINT16    PaddingData;
  UINT16    PaddingHdr;
  UINT32    Reserved;
} SMEM_PRIVATE_ENTRY;

typedef struct {
  CHAR8     Name[SMP2P_MAX_ENTRY_NAME];
  UINT32    Value;
} SMP2P_ENTRY;

typedef struct {
  UINT32         Magic;
  UINT8          Version;
  UINT8          Features[3];
  UINT16         LocalPid;
  UINT16         RemotePid;
  UINT16         TotalEntries;
  UINT16         ValidEntries;
  UINT32         Flags;
  SMP2P_ENTRY    Entries[SMP2P_MAX_ENTRY];
} SMP2P_SMEM_ITEM;

#pragma pack()

STATIC UINT8   *mSmemBase;
STATIC UINT64  mSmemSize;

EFI_STATUS
SmemInit (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mSmemBase != NULL) {
    return EFI_SUCCESS;
  }

  mSmemSize = PcdGet32 (PcdSmemSize);
  if ((PcdGet64 (PcdSmemBaseAddress) == 0) || (mSmemSize < SIZE_4KB)) {
    return EFI_NOT_FOUND;
  }

  Status = DspSetMemoryAttributes (PcdGet64 (PcdSmemBaseAddress), mSmemSize, EFI_MEMORY_WC | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot map SMEM: %r\n", __func__, Status));
    return Status;
  }

  mSmemBase = (UINT8 *)(UINTN)PcdGet64 (PcdSmemBaseAddress);
  return EFI_SUCCESS;
}

/**
  Returns the partition table, if SMEM has one.

  @return  The table, or NULL.
**/
STATIC
CONST SMEM_PTABLE *
SmemGetPtable (
  VOID
  )
{
  CONST SMEM_PTABLE  *Ptable;

  Ptable = (CONST SMEM_PTABLE *)(mSmemBase + mSmemSize - SIZE_4KB);
  if (Ptable->Magic != SMEM_PTABLE_MAGIC) {
    return NULL;
  }

  if ((Ptable->NumEntries == 0) ||
      (sizeof (SMEM_PTABLE) + (UINT64)Ptable->NumEntries * sizeof (SMEM_PTABLE_ENTRY) > SIZE_4KB))
  {
    return NULL;
  }

  return Ptable;
}

/**
  Finds the partition two hosts share, and checks its header the way
  qcom_smem_partition_header() does.

  @param[in]   Host0      One host.
  @param[in]   Host1      The other.
  @param[out]  Header     The partition header.
  @param[out]  Cacheline  The partition's cache line size.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  No such partition, or a damaged one.
**/
STATIC
EFI_STATUS
SmemFindPartition (
  IN  UINT16                       Host0,
  IN  UINT16                       Host1,
  OUT CONST SMEM_PARTITION_HEADER  **Header,
  OUT UINT32                       *Cacheline
  )
{
  CONST SMEM_PTABLE            *Ptable;
  CONST SMEM_PTABLE_ENTRY      *Entry;
  CONST SMEM_PARTITION_HEADER  *Phdr;
  UINTN                        Index;

  Ptable = SmemGetPtable ();
  if (Ptable == NULL) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < Ptable->NumEntries; Index++) {
    Entry = &Ptable->Entry[Index];
    if ((Entry->Offset == 0) || (Entry->Size == 0)) {
      continue;
    }

    if (!(((Entry->Host0 == Host0) && (Entry->Host1 == Host1)) ||
          ((Entry->Host0 == Host1) && (Entry->Host1 == Host0))))
    {
      continue;
    }

    if ((UINT64)Entry->Offset + Entry->Size > mSmemSize) {
      return EFI_NOT_FOUND;
    }

    Phdr = (CONST SMEM_PARTITION_HEADER *)(mSmemBase + Entry->Offset);
    if ((Phdr->Magic != SMEM_PART_MAGIC) ||
        (Phdr->Host0 != Entry->Host0) || (Phdr->Host1 != Entry->Host1) ||
        (Phdr->Size != Entry->Size) ||
        (Phdr->OffsetFreeUncached > Phdr->Size) ||
        (Phdr->OffsetFreeCached > Phdr->Size))
    {
      DEBUG ((DEBUG_ERROR, "%a: bad partition header for hosts %u/%u\n", __func__, Host0, Host1));
      return EFI_NOT_FOUND;
    }

    *Header    = Phdr;
    *Cacheline = Entry->Cacheline;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

/**
  Finds an item in a partition (qcom_smem_get_private).

  @param[in]   Phdr       The partition header.
  @param[in]   Cacheline  Its cache line size.
  @param[in]   Item       The item.
  @param[out]  Data       Its data.
  @param[out]  Size       Its size.

  @retval EFI_SUCCESS            Found.
  @retval EFI_NOT_FOUND          Not there.
  @retval EFI_VOLUME_CORRUPTED   The partition is damaged.
**/
STATIC
EFI_STATUS
SmemFindPrivateItem (
  IN  CONST SMEM_PARTITION_HEADER  *Phdr,
  IN  UINT32                       Cacheline,
  IN  UINT32                       Item,
  OUT VOID                         **Data,
  OUT UINTN                        *Size
  )
{
  CONST UINT8               *Start;
  CONST UINT8               *End;
  CONST UINT8               *Cursor;
  CONST SMEM_PRIVATE_ENTRY  *Entry;
  UINTN                     EntryStride;

  Start = (CONST UINT8 *)Phdr;
  End   = Start + Phdr->Size;

  //
  // Uncached list: entry headers followed by their data, upwards.
  //
  Cursor = Start + sizeof (SMEM_PARTITION_HEADER);
  while (Cursor + sizeof (SMEM_PRIVATE_ENTRY) <= Start + Phdr->OffsetFreeUncached) {
    Entry = (CONST SMEM_PRIVATE_ENTRY *)Cursor;
    if (Entry->Canary != SMEM_PRIVATE_CANARY) {
      return EFI_VOLUME_CORRUPTED;
    }

    if ((Entry->Size > Phdr->Size) || (Entry->PaddingData > Entry->Size)) {
      return EFI_VOLUME_CORRUPTED;
    }

    if (Entry->Item == Item) {
      if (Cursor + sizeof (SMEM_PRIVATE_ENTRY) + Entry->PaddingHdr + Entry->Size > End) {
        return EFI_VOLUME_CORRUPTED;
      }

      *Data = (VOID *)(Cursor + sizeof (SMEM_PRIVATE_ENTRY) + Entry->PaddingHdr);
      *Size = Entry->Size - Entry->PaddingData;
      return EFI_SUCCESS;
    }

    Cursor += sizeof (SMEM_PRIVATE_ENTRY) + Entry->PaddingHdr + Entry->Size;
  }

  //
  // Cached list: entry headers with their data below them, downwards from
  // the end of the partition.
  //
  if ((Cacheline == 0) || (Cacheline > SIZE_4KB)) {
    return EFI_NOT_FOUND;
  }

  EntryStride = ALIGN_VALUE (sizeof (SMEM_PRIVATE_ENTRY), Cacheline);
  if (EntryStride > Phdr->Size) {
    return EFI_NOT_FOUND;
  }

  Cursor = End - EntryStride;
  while (Cursor > Start + Phdr->OffsetFreeCached) {
    Entry = (CONST SMEM_PRIVATE_ENTRY *)Cursor;
    if (Entry->Canary != SMEM_PRIVATE_CANARY) {
      return EFI_VOLUME_CORRUPTED;
    }

    if ((Entry->Size > Phdr->Size) || (Entry->PaddingData > Entry->Size) ||
        ((UINTN)(Cursor - Start) < Entry->Size))
    {
      return EFI_VOLUME_CORRUPTED;
    }

    if (Entry->Item == Item) {
      *Data = (VOID *)(Cursor - Entry->Size);
      *Size = Entry->Size - Entry->PaddingData;
      return EFI_SUCCESS;
    }

    if ((UINTN)(Cursor - Start) < Entry->Size + EntryStride) {
      break;
    }

    Cursor -= Entry->Size + EntryStride;
  }

  return EFI_NOT_FOUND;
}

/**
  Finds the SMP2P entry a remote processor sets for apps ("slave-kernel"),
  as qcom_smp2p_start_in() does.

  @param[in]   RemoteHost  The remote processor's SMEM host ID.
  @param[in]   Item        The SMEM item of its SMP2P entries to apps.
  @param[out]  Value       Address of the entry's value.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  The remote has not created the item or the entry.
  @retval Other          SMEM is not usable.
**/
STATIC
EFI_STATUS
SmemFindSmp2pInbound (
  IN  UINT16  RemoteHost,
  IN  UINT32  Item,
  OUT UINTN   *Value
  )
{
  EFI_STATUS                   Status;
  CONST SMEM_PARTITION_HEADER  *Phdr;
  UINT32                       Cacheline;
  VOID                         *Data;
  UINTN                        Size;
  CONST SMP2P_SMEM_ITEM        *Smp2p;
  UINTN                        Index;
  UINTN                        Valid;

  if (mSmemBase == NULL) {
    return EFI_NOT_READY;
  }

  Status = SmemFindPartition (SMEM_HOST_APPS, RemoteHost, &Phdr, &Cacheline);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmemFindPrivateItem (Phdr, Cacheline, Item, &Data, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Size < OFFSET_OF (SMP2P_SMEM_ITEM, Entries)) {
    return EFI_NOT_FOUND;
  }

  //
  // A version of 0 means the remote has not set the item up yet
  // (qcom_smp2p_start_in).
  //
  Smp2p = (CONST SMP2P_SMEM_ITEM *)Data;
  if ((Smp2p->Magic != SMP2P_MAGIC) || (Smp2p->Version == 0)) {
    return EFI_NOT_FOUND;
  }

  Valid = MIN (Smp2p->ValidEntries, SMP2P_MAX_ENTRY);
  Valid = MIN (Valid, (Size - OFFSET_OF (SMP2P_SMEM_ITEM, Entries)) / sizeof (SMP2P_ENTRY));
  for (Index = 0; Index < Valid; Index++) {
    if (AsciiStrnCmp (Smp2p->Entries[Index].Name, SMP2P_INBOUND_ENTRY, SMP2P_MAX_ENTRY_NAME) == 0) {
      *Value = (UINTN)&Smp2p->Entries[Index].Value;
      return EFI_SUCCESS;
    }
  }

  //
  // The remote is up, but has not added its entries yet.
  //
  return EFI_NOT_READY;
}

EFI_STATUS
SmemReadSmp2pInbound (
  IN  UINT16  RemoteHost,
  IN  UINT32  Item,
  OUT UINT32  *Bits
  )
{
  EFI_STATUS  Status;
  UINTN       Value;

  Status = SmemFindSmp2pInbound (RemoteHost, Item, &Value);
  if (!EFI_ERROR (Status)) {
    *Bits = MmioRead32 (Value);
  }

  return Status;
}

EFI_STATUS
SmemClearSmp2pInbound (
  IN UINT16  RemoteHost,
  IN UINT32  Item
  )
{
  EFI_STATUS  Status;
  UINTN       Value;

  Status = SmemFindSmp2pInbound (RemoteHost, Item, &Value);
  if (!EFI_ERROR (Status)) {
    MmioWrite32 (Value, 0);
    MemoryFence ();
  }

  return Status;
}

EFI_STATUS
SmemGetGlobalItem (
  IN  UINT32  Item,
  OUT VOID    **Data,
  OUT UINTN   *Size
  )
{
  EFI_STATUS                   Status;
  CONST SMEM_HEADER            *Header;
  CONST SMEM_GLOBAL_ENTRY      *Entry;
  CONST SMEM_PARTITION_HEADER  *Phdr;
  UINT32                       Cacheline;

  if (mSmemBase == NULL) {
    return EFI_NOT_READY;
  }

  Header = (CONST SMEM_HEADER *)mSmemBase;
  if ((Header->Version[SMEM_MASTER_SBL_VERSION_INDEX] >> 16) >= SMEM_GLOBAL_PART_VERSION) {
    Status = SmemFindPartition (SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST, &Phdr, &Cacheline);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    return SmemFindPrivateItem (Phdr, Cacheline, Item, Data, Size);
  }

  //
  // Older SMEM: a table of contents of the global heap after the header.
  // Only items in the main region (no auxiliary base) are looked at.
  //
  if (OFFSET_OF (SMEM_HEADER, Toc) + (UINT64)(Item + 1) * sizeof (SMEM_GLOBAL_ENTRY) > mSmemSize) {
    return EFI_NOT_FOUND;
  }

  Entry = &Header->Toc[Item];
  if ((Entry->Allocated == 0) || ((Entry->AuxBase & ~(UINT32)0x3) != 0) ||
      ((UINT64)Entry->Offset + Entry->Size > mSmemSize))
  {
    return EFI_NOT_FOUND;
  }

  *Data = mSmemBase + Entry->Offset;
  *Size = Entry->Size;
  return EFI_SUCCESS;
}

/**
  Takes the SMEM hardware lock.

  @return  TRUE if taken.
**/
STATIC
BOOLEAN
SmemLock (
  VOID
  )
{
  UINTN  Waited;

  for (Waited = 0; Waited < SMEM_HWLOCK_TIMEOUT; Waited++) {
    MmioWrite32 (SMEM_HWLOCK_ADDR, SMEM_HWLOCK_APPS);
    if (MmioRead32 (SMEM_HWLOCK_ADDR) == SMEM_HWLOCK_APPS) {
      return TRUE;
    }

    MicroSecondDelay (1);
  }

  return FALSE;
}

/**
  Releases the SMEM hardware lock.
**/
STATIC
VOID
SmemUnlock (
  VOID
  )
{
  MemoryFence ();
  MmioWrite32 (SMEM_HWLOCK_ADDR, 0);
}

/**
  Allocates an item in the partition apps shares with a remote host, or
  finds it if it is there (qcom_smem_alloc_private).

  @param[in]   RemoteHost  The remote host.
  @param[in]   Item        The item.
  @param[in]   Size        Its size.
  @param[out]  Data        Its data.

  @retval EFI_SUCCESS          Allocated.
  @retval EFI_ALREADY_STARTED  It was there already.
  @retval Other                It could not be allocated.
**/
STATIC
EFI_STATUS
SmemAllocPrivate (
  IN  UINT16  RemoteHost,
  IN  UINT32  Item,
  IN  UINTN   Size,
  OUT VOID    **Data
  )
{
  EFI_STATUS             Status;
  SMEM_PARTITION_HEADER  *Phdr;
  UINT32                 Cacheline;
  UINT8                  *Cursor;
  SMEM_PRIVATE_ENTRY     *Entry;
  UINTN                  AllocSize;
  UINTN                  Existing;

  Status = SmemFindPartition (SMEM_HOST_APPS, RemoteHost, (CONST SMEM_PARTITION_HEADER **)&Phdr, &Cacheline);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!SmemLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: SMEM lock busy\n", __func__));
    return EFI_TIMEOUT;
  }

  Status = SmemFindPrivateItem (Phdr, Cacheline, Item, Data, &Existing);
  if (!EFI_ERROR (Status)) {
    SmemUnlock ();
    return (Existing >= Size) ? EFI_ALREADY_STARTED : EFI_BAD_BUFFER_SIZE;
  }

  if (Status != EFI_NOT_FOUND) {
    SmemUnlock ();
    return Status;
  }

  //
  // A new entry at the end of the uncached list, which must not grow into
  // the cached list.
  //
  AllocSize = sizeof (SMEM_PRIVATE_ENTRY) + ALIGN_VALUE (Size, 8);
  Cursor    = (UINT8 *)Phdr + Phdr->OffsetFreeUncached;
  if (Phdr->OffsetFreeUncached + AllocSize > Phdr->OffsetFreeCached) {
    SmemUnlock ();
    return EFI_OUT_OF_RESOURCES;
  }

  Entry              = (SMEM_PRIVATE_ENTRY *)Cursor;
  Entry->Canary      = SMEM_PRIVATE_CANARY;
  Entry->Item        = (UINT16)Item;
  Entry->Size        = (UINT32)ALIGN_VALUE (Size, 8);
  Entry->PaddingData = (UINT16)(Entry->Size - Size);
  Entry->PaddingHdr  = 0;
  Entry->Reserved    = 0;

  //
  // The entry first, then the free offset that makes it part of the list.
  //
  MemoryFence ();
  Phdr->OffsetFreeUncached += (UINT32)AllocSize;
  SmemUnlock ();

  *Data = Cursor + sizeof (SMEM_PRIVATE_ENTRY);
  return EFI_SUCCESS;
}

EFI_STATUS
SmemCreateSmp2pOutbound (
  IN UINT16  RemoteHost,
  IN UINT32  Item,
  IN UINT32  InboundItem,
  IN UINT32  IpccSignal
  )
{
  EFI_STATUS                   Status;
  SMP2P_SMEM_ITEM              *Out;
  VOID                         *Data;
  VOID                         *InData;
  UINTN                        InSize;
  UINT8                        Version;
  UINTN                        Index;
  CONST SMEM_PARTITION_HEADER  *Phdr;
  UINT32                       Cacheline;

  if (mSmemBase == NULL) {
    return EFI_NOT_READY;
  }

  Status = SmemAllocPrivate (RemoteHost, Item, sizeof (SMP2P_SMEM_ITEM), &Data);
  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: host %u item %u %a\n",
    __func__,
    RemoteHost,
    Item,
    (Status == EFI_ALREADY_STARTED) ? "already there, set up again" : "allocated"
    ));

  //
  // The header Linux's qcom_smp2p_alloc_outbound_item() writes, with the
  // one entry its device tree gives apps; the version last, matched to the
  // remote's if it has one.
  //
  Out = Data;
  ZeroMem (Out, sizeof (*Out));
  Out->Magic        = SMP2P_MAGIC;
  Out->LocalPid     = SMEM_HOST_APPS;
  Out->RemotePid    = RemoteHost;
  Out->TotalEntries = SMP2P_MAX_ENTRY;
  Out->Features[0]  = SMP2P_FEATURE_SSR_ACK;
  for (Index = 0; Index < sizeof (SMP2P_OUTBOUND_ENTRY) - 1; Index++) {
    Out->Entries[0].Name[Index] = SMP2P_OUTBOUND_ENTRY[Index];
  }

  Out->Entries[0].Value = 0;
  Out->ValidEntries     = 1;

  Version = SMP2P_MAX_VERSION;
  if (!EFI_ERROR (SmemFindPartition (SMEM_HOST_APPS, RemoteHost, &Phdr, &Cacheline))) {
    if (!EFI_ERROR (SmemFindPrivateItem (Phdr, Cacheline, InboundItem, &InData, &InSize)) &&
        (InSize >= OFFSET_OF (SMP2P_SMEM_ITEM, Entries)) &&
        (((SMP2P_SMEM_ITEM *)InData)->Version != 0) &&
        (((SMP2P_SMEM_ITEM *)InData)->Version <= SMP2P_MAX_VERSION))
    {
      Version = ((SMP2P_SMEM_ITEM *)InData)->Version;
    }
  }

  MemoryFence ();
  Out->Version = Version;
  MemoryFence ();

  //
  // Tell the remote.
  //
  MmioWrite32 (IPCC_SEND_ID, IpccSignal);
  return EFI_SUCCESS;
}

VOID
SmemLogPartition (
  IN UINT16  RemoteHost
  )
{
  CONST SMEM_PARTITION_HEADER  *Phdr;
  UINT32                       Cacheline;
  CONST UINT8                  *Cursor;
  CONST SMEM_PRIVATE_ENTRY     *Entry;
  CONST SMP2P_SMEM_ITEM        *Smp2p;
  UINTN                        Count;
  UINTN                        Index;

  if ((mSmemBase == NULL) || EFI_ERROR (SmemFindPartition (SMEM_HOST_APPS, RemoteHost, &Phdr, &Cacheline))) {
    DEBUG ((DEBUG_ERROR, "%a: no partition for host %u\n", __func__, RemoteHost));
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "%a: host %u partition at 0x%lx, 0x%x bytes, free uncached 0x%x cached 0x%x\n",
    __func__,
    RemoteHost,
    (UINT64)(UINTN)Phdr,
    Phdr->Size,
    Phdr->OffsetFreeUncached,
    Phdr->OffsetFreeCached
    ));

  Cursor = (CONST UINT8 *)Phdr + sizeof (SMEM_PARTITION_HEADER);
  for (Count = 0; Count < 64 && Cursor + sizeof (SMEM_PRIVATE_ENTRY) <= (CONST UINT8 *)Phdr + Phdr->OffsetFreeUncached; Count++) {
    Entry = (CONST SMEM_PRIVATE_ENTRY *)Cursor;
    if ((Entry->Canary != SMEM_PRIVATE_CANARY) || (Entry->Size > Phdr->Size)) {
      DEBUG ((DEBUG_ERROR, "%a:   bad entry at +0x%x\n", __func__, (UINT32)(Cursor - (CONST UINT8 *)Phdr)));
      break;
    }

    DEBUG ((DEBUG_ERROR, "%a:   uncached item %u, %u bytes\n", __func__, Entry->Item, Entry->Size - Entry->PaddingData));

    Smp2p = (CONST SMP2P_SMEM_ITEM *)(Cursor + sizeof (SMEM_PRIVATE_ENTRY) + Entry->PaddingHdr);
    if ((Entry->Size >= sizeof (SMP2P_SMEM_ITEM)) && (Smp2p->Magic == SMP2P_MAGIC)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a:     SMP2P %u->%u v%u features 0x%x flags 0x%x, %u entries\n",
        __func__,
        Smp2p->LocalPid,
        Smp2p->RemotePid,
        Smp2p->Version,
        Smp2p->Features[0],
        Smp2p->Flags,
        Smp2p->ValidEntries
        ));
      for (Index = 0; Index < MIN (Smp2p->ValidEntries, SMP2P_MAX_ENTRY); Index++) {
        DEBUG ((DEBUG_ERROR, "%a:       %.16a = 0x%x\n", __func__, Smp2p->Entries[Index].Name, Smp2p->Entries[Index].Value));
      }
    }

    Cursor += sizeof (SMEM_PRIVATE_ENTRY) + Entry->PaddingHdr + Entry->Size;
  }

  if (Phdr->OffsetFreeCached < Phdr->Size) {
    DEBUG ((DEBUG_ERROR, "%a:   0x%x bytes of cached items\n", __func__, Phdr->Size - Phdr->OffsetFreeCached));
  }
}

/**
  Finds an SMP2P item and checks its header.

  @param[in]   RemoteHost  The remote host whose partition holds it.
  @param[in]   Item        The item.
  @param[out]  Smp2p       The item.

  @retval EFI_SUCCESS    Found, set up (version not 0).
  @retval EFI_NOT_FOUND  Not there, or not set up yet.
**/
STATIC
EFI_STATUS
SmemGetSmp2pItem (
  IN  UINT16           RemoteHost,
  IN  UINT32           Item,
  OUT SMP2P_SMEM_ITEM  **Smp2p
  )
{
  CONST SMEM_PARTITION_HEADER  *Phdr;
  UINT32                       Cacheline;
  VOID                         *Data;
  UINTN                        Size;

  if ((mSmemBase == NULL) ||
      EFI_ERROR (SmemFindPartition (SMEM_HOST_APPS, RemoteHost, &Phdr, &Cacheline)) ||
      EFI_ERROR (SmemFindPrivateItem (Phdr, Cacheline, Item, &Data, &Size)) ||
      (Size < OFFSET_OF (SMP2P_SMEM_ITEM, Entries)))
  {
    return EFI_NOT_FOUND;
  }

  *Smp2p = Data;
  if (((*Smp2p)->Magic != SMP2P_MAGIC) || ((*Smp2p)->Version == 0)) {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
SmemNegotiateSmp2p (
  IN UINT16  RemoteHost,
  IN UINT32  Item,
  IN UINT32  InboundItem,
  IN UINT32  IpccSignal
  )
{
  SMP2P_SMEM_ITEM  *In;
  SMP2P_SMEM_ITEM  *Out;

  if (EFI_ERROR (SmemGetSmp2pItem (RemoteHost, InboundItem, &In)) ||
      EFI_ERROR (SmemGetSmp2pItem (RemoteHost, Item, &Out)))
  {
    return EFI_NOT_READY;
  }

  //
  // qcom_smp2p_negotiate(): the side with the higher version comes down to
  // the other's and tells it; with the same version, the features both
  // have are kept, and the remote goes on to add its entries.
  //
  if (In->Version == Out->Version) {
    Out->Features[0] &= In->Features[0];
    Out->Features[1] &= In->Features[1];
    Out->Features[2] &= In->Features[2];
    return EFI_SUCCESS;
  }

  if (In->Version < Out->Version) {
    DEBUG ((DEBUG_INFO, "%a: host %u has SMP2P v%u, apps comes down from v%u\n", __func__, RemoteHost, In->Version, Out->Version));
    Out->Version = In->Version;
    MemoryFence ();
    MmioWrite32 (IPCC_SEND_ID, IpccSignal);
  }

  return EFI_NOT_READY;
}

