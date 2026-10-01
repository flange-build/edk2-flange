/** @file
  Qualcomm MDT/MBN firmware images: the checks, metadata and segment loading
  of Linux's drivers/soc/qcom/mdt_loader.c.

  An image is a 32-bit ELF file. Program header 0 covers the ELF and program
  headers; the hash segment (type 2 in bits 26:24 of p_flags) holds a hash of
  every segment and the signature. TrustZone authenticates the metadata,
  which is the headers followed by the hash segment, then the loaded
  segments. Segments with bit 27 of p_flags set may be placed elsewhere than
  their physical address; TrustZone is told where (PAS mem_setup).

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/MemoryAllocationLib.h>

#include "DspPreload.h"

//
// 32-bit ELF, as far as an MDT image needs it (elf.h).
//
#define ELF_MAGIC      "\177ELF"
#define ELF_MAGIC_LEN  4
#define PT_LOAD        1

typedef struct {
  UINT8     e_ident[16];
  UINT16    e_type;
  UINT16    e_machine;
  UINT32    e_version;
  UINT32    e_entry;
  UINT32    e_phoff;
  UINT32    e_shoff;
  UINT32    e_flags;
  UINT16    e_ehsize;
  UINT16    e_phentsize;
  UINT16    e_phnum;
  UINT16    e_shentsize;
  UINT16    e_shnum;
  UINT16    e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  UINT32    p_type;
  UINT32    p_offset;
  UINT32    p_vaddr;
  UINT32    p_paddr;
  UINT32    p_filesz;
  UINT32    p_memsz;
  UINT32    p_flags;
  UINT32    p_align;
} Elf32_Phdr;

#define ELF32_SHDR_SIZE  40

#define MDT_TYPE_MASK    (7U << 24)
#define MDT_TYPE_HASH    (2U << 24)
#define MDT_RELOCATABLE  BIT27

#define MDT_ALIGN  SIZE_4KB

//
// The version string Qualcomm build tools put in their images.
//
#define MDT_VERSION_KEY  "QC_IMAGE_VERSION_STRING="

/**
  Returns whether a segment is loaded (mdt_phdr_loadable).

  @param[in]  Phdr  The program header.

  @return  TRUE if it is.
**/
STATIC
BOOLEAN
MdtSegmentLoadable (
  IN CONST Elf32_Phdr  *Phdr
  )
{
  return (Phdr->p_type == PT_LOAD) &&
         ((Phdr->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH) &&
         (Phdr->p_memsz != 0);
}

/**
  Checks the ELF header and that the program and section header tables lie
  in the file (mdt_header_valid).

  @param[in]  Data  The file.
  @param[in]  Size  Its size.

  @return  TRUE if valid.
**/
STATIC
BOOLEAN
MdtHeaderValid (
  IN CONST UINT8  *Data,
  IN UINTN        Size
  )
{
  CONST Elf32_Ehdr  *Ehdr;

  if (Size < sizeof (Elf32_Ehdr)) {
    return FALSE;
  }

  Ehdr = (CONST Elf32_Ehdr *)Data;
  if (CompareMem (Ehdr->e_ident, ELF_MAGIC, ELF_MAGIC_LEN) != 0) {
    return FALSE;
  }

  if (Ehdr->e_phentsize != sizeof (Elf32_Phdr)) {
    return FALSE;
  }

  if ((UINT64)Ehdr->e_phoff + (UINT64)Ehdr->e_phnum * sizeof (Elf32_Phdr) > Size) {
    return FALSE;
  }

  if ((Ehdr->e_shentsize != 0) || (Ehdr->e_shnum != 0)) {
    if (Ehdr->e_shentsize != ELF32_SHDR_SIZE) {
      return FALSE;
    }

    if ((UINT64)Ehdr->e_shoff + (UINT64)Ehdr->e_shnum * ELF32_SHDR_SIZE > Size) {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Copies the image's version string, if it has one, for the log.

  @param[in,out]  Image  The image.
**/
STATIC
VOID
MdtFindVersion (
  IN OUT DSP_IMAGE  *Image
  )
{
  UINTN  KeyLength;
  UINTN  Offset;
  UINTN  Length;

  KeyLength = sizeof (MDT_VERSION_KEY) - 1;
  for (Offset = 0; Offset + KeyLength < Image->Size; Offset++) {
    if ((Image->Data[Offset] == MDT_VERSION_KEY[0]) &&
        (CompareMem (&Image->Data[Offset], MDT_VERSION_KEY, KeyLength) == 0))
    {
      Offset += KeyLength;
      for (Length = 0;
           (Length < sizeof (Image->Version) - 1) && (Offset + Length < Image->Size) &&
           (Image->Data[Offset + Length] >= 0x20) && (Image->Data[Offset + Length] < 0x7F);
           Length++)
      {
        Image->Version[Length] = (CHAR8)Image->Data[Offset + Length];
      }

      Image->Version[Length] = '\0';
      return;
    }
  }
}

EFI_STATUS
MdtParse (
  IN OUT DSP_IMAGE  *Image
  )
{
  CONST Elf32_Ehdr  *Ehdr;
  CONST Elf32_Phdr  *Phdrs;
  CONST Elf32_Phdr  *Phdr;
  UINTN             Index;
  UINTN             HashSegment;
  UINT64            HeaderSize;
  UINT64            HashSize;
  UINT64            HashOffset;

  Image->Relocatable = FALSE;
  Image->MinAddr     = MAX_UINT64;
  Image->MaxAddr     = 0;

  if (!MdtHeaderValid (Image->Data, Image->Size)) {
    return EFI_VOLUME_CORRUPTED;
  }

  Ehdr  = (CONST Elf32_Ehdr *)Image->Data;
  Phdrs = (CONST Elf32_Phdr *)(Image->Data + Ehdr->e_phoff);

  if ((Ehdr->e_phnum < 2) || (Phdrs[0].p_type == PT_LOAD)) {
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // The span of the loadable segments, computed as __qcom_mdt_pas_init()
  // does; and an image whose segments are not all in the file is split.
  //
  for (Index = 0; Index < Ehdr->e_phnum; Index++) {
    Phdr = &Phdrs[Index];

    if ((Phdr->p_filesz != 0) && ((UINT64)Phdr->p_offset + Phdr->p_filesz > Image->Size)) {
      return EFI_UNSUPPORTED;
    }

    if (!MdtSegmentLoadable (Phdr)) {
      continue;
    }

    if ((Phdr->p_flags & MDT_RELOCATABLE) != 0) {
      Image->Relocatable = TRUE;
    }

    if (Phdr->p_paddr < Image->MinAddr) {
      Image->MinAddr = Phdr->p_paddr;
    }

    if ((UINT64)Phdr->p_paddr + Phdr->p_memsz > Image->MaxAddr) {
      Image->MaxAddr = ALIGN_VALUE ((UINT64)Phdr->p_paddr + Phdr->p_memsz, MDT_ALIGN);
    }
  }

  if (Image->MaxAddr == 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  for (HashSegment = 1; HashSegment < Ehdr->e_phnum; HashSegment++) {
    if ((Phdrs[HashSegment].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
      break;
    }
  }

  if (HashSegment == Ehdr->e_phnum) {
    DEBUG ((DEBUG_ERROR, "%a: no hash segment\n", __func__));
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // The metadata (qcom_mdt_read_metadata): the headers, then the hash
  // segment, from right after the headers in a split .mdt file, or from where
  // the segment lies in the file.
  //
  HeaderSize = Phdrs[0].p_filesz;
  HashSize   = Phdrs[HashSegment].p_filesz;
  if ((HeaderSize == 0) || (HeaderSize > Image->Size) || (HashSize == 0)) {
    return EFI_VOLUME_CORRUPTED;
  }

  if (HeaderSize + HashSize == Image->Size) {
    HashOffset = HeaderSize;
  } else if ((UINT64)Phdrs[HashSegment].p_offset + HashSize <= Image->Size) {
    HashOffset = Phdrs[HashSegment].p_offset;
  } else {
    return EFI_UNSUPPORTED;
  }

  Image->MetadataSize = (UINTN)(HeaderSize + HashSize);
  Image->Metadata     = AllocatePool (Image->MetadataSize);
  if (Image->Metadata == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Image->Metadata, Image->Data, (UINTN)HeaderSize);
  CopyMem (Image->Metadata + HeaderSize, Image->Data + HashOffset, (UINTN)HashSize);

  MdtFindVersion (Image);

  DEBUG ((
    DEBUG_INFO,
    "%a: %u program headers, metadata 0x%x bytes, segments 0x%lx-0x%lx%a, %a\n",
    __func__,
    Ehdr->e_phnum,
    (UINT32)Image->MetadataSize,
    Image->MinAddr,
    Image->MaxAddr,
    Image->Relocatable ? " (relocatable)" : "",
    (Image->Version[0] != '\0') ? Image->Version : "no version string"
    ));

  return EFI_SUCCESS;
}

EFI_STATUS
MdtLoadSegments (
  IN CONST DSP_IMAGE       *Image,
  IN VOID                  *Region,
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  CONST Elf32_Ehdr      *Ehdr;
  CONST Elf32_Phdr      *Phdrs;
  CONST Elf32_Phdr      *Phdr;
  EFI_PHYSICAL_ADDRESS  Reloc;
  UINTN                 Index;
  UINT64                Offset;
  UINT8                 *Ptr;

  Ehdr  = (CONST Elf32_Ehdr *)Image->Data;
  Phdrs = (CONST Elf32_Phdr *)(Image->Data + Ehdr->e_phoff);

  //
  // A relocatable image goes in at its lowest segment, a fixed one where
  // its addresses say, relative to the memory it is given.
  //
  Reloc = Image->Relocatable ? Image->MinAddr : Base;

  for (Index = 0; Index < Ehdr->e_phnum; Index++) {
    Phdr = &Phdrs[Index];

    if (!MdtSegmentLoadable (Phdr)) {
      continue;
    }

    if ((Phdr->p_paddr < Reloc) || ((UINT64)Phdr->p_paddr - Reloc + Phdr->p_memsz > Size)) {
      DEBUG ((DEBUG_ERROR, "%a: segment %u (0x%x, 0x%x bytes) outside the memory\n", __func__, (UINT32)Index, Phdr->p_paddr, Phdr->p_memsz));
      return EFI_VOLUME_CORRUPTED;
    }

    if (Phdr->p_filesz > Phdr->p_memsz) {
      DEBUG ((DEBUG_ERROR, "%a: segment %u has more file than memory\n", __func__, (UINT32)Index));
      return EFI_VOLUME_CORRUPTED;
    }

    Offset = (UINT64)Phdr->p_paddr - Reloc;
    Ptr    = (UINT8 *)Region + Offset;

    if (Phdr->p_filesz != 0) {
      CopyMem (Ptr, Image->Data + Phdr->p_offset, Phdr->p_filesz);
    }

    if (Phdr->p_memsz > Phdr->p_filesz) {
      ZeroMem (Ptr + Phdr->p_filesz, Phdr->p_memsz - Phdr->p_filesz);
    }
  }

  return EFI_SUCCESS;
}

VOID
MdtFree (
  IN OUT DSP_IMAGE  *Image
  )
{
  if (Image->Metadata != NULL) {
    FreePool (Image->Metadata);
    Image->Metadata = NULL;
  }

  if (Image->Data != NULL) {
    FreePool (Image->Data);
    Image->Data = NULL;
  }
}
