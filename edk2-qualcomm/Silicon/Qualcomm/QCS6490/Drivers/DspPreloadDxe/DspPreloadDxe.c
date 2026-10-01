/** @file
  Loads the ADSP and CDSP firmware and starts the DSPs before the OS, for a
  kernel that attaches to running DSPs.

  At EL2 Linux cannot start the DSPs itself on this board: with no
  hypervisor, Linux 7.0 asks TrustZone for a resource table that this
  TrustZone does not provide. Its PAS driver does attach to a DSP the boot
  firmware started (it reads ready and handover in the DSP's SMP2P entry
  when it probes), which is how the Radxa Dragon Q6A runs its DSPs at EL2.

  TrustZone only runs the DSPs it starts for a Gunyah guest: at EL2 it
  accepts every PAS call, but the DSPs never run. So in a boot that runs the
  OS at EL2, SEC keeps Gunyah until ExitBootServices (GunyahExitDxe) when
  the DSPs are to be preloaded, and they are started from EL1.

  When the boot manager is about to boot (ReadyToBoot), and the DspPreload
  setting asks for it, each DSP of the board's device tree is started the
  way Linux's qcom_q6v5_pas starts it:

  1. The firmware named in its remoteproc node is read from the OS's root
     file system and checked (Firmware.c, Mdt.c).
  2. At EL2, its apps SMMU stream is handed over in bypass (SmmuDxe); when
     Gunyah leaves at ExitBootServices, that happens once it has left, for
     the DSPs that run.
  3. AOP is told its image is loaded (QMP load_state), and its power rails,
     and the CDSP's path to memory, are voted at their highest level, as
     Linux's proxy votes do.
  4. TrustZone gets the image's metadata (PAS init_image) and where the
     image goes (mem_setup); the segments are copied into the carve-out and
     TrustZone authenticates them and releases the DSP (auth_and_reset).
  5. The DSP's SMP2P entry is polled until it says ready and handover; then
     the proxy votes are put back and the DSP is recorded in the PIL
     relocation table in IMEM.

  A DSP that fails is shut down again through PAS. The DSPs that run have the
  iommus of their remoteproc nodes removed from the device tree the OS gets
  (DeviceTree.c), as Linux would otherwise put their streams in an empty
  SMMU domain.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Guid/Qcs6490PlatformConfig.h>
#include <Library/ArmLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/Qcs6490RpmhLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/Qcs6490NvStatusLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/Qcs6490GunyahExit.h>
#include <Protocol/Qcs6490Smmu.h>

#include "DspPreload.h"

//
// Linux waits five seconds for a remote processor to say ready
// (qcom_q6v5_wait_for_start).
//
#define DSP_START_TIMEOUT_MS  5000
#define DSP_POLL_INTERVAL_US  1000

//
// apps_smmu in kodiak.dtsi, for the fault log.
//
#define DSP_APPS_SMMU_BASE  0x15000000

//
// The XO clock's RPMh resource, and its "on" level (clk-rpmh.c bi_tcxo,
// "xo.lvl" voted 3), which Linux holds for a DSP while it boots.
//
#define DSP_XO_RESOURCE       "xo.lvl"
#define DSP_XO_FALLBACK_ADDR  0x300B0
#define DSP_XO_ON             3

//
// PIL relocation table in IMEM (kodiak.dtsi pil-reloc@594c,
// qcom_pil_info.c): 20-byte entries of an 8-character name, a 64-bit base
// and a 32-bit size, packed from the start.
//
#define PIL_RELOC_BASE        0x146AA94C
#define PIL_RELOC_SIZE        200
#define PIL_RELOC_ENTRY_SIZE  20
#define PIL_RELOC_NAME_LEN    8

//
// The QCS6490 DSPs. Bits, items and streams from kodiak.dtsi and Linux's
// qcom_q6v5_pas.c (sm8350_adsp_resource, sm6350_cdsp_resource); rails and
// interconnect from the proxy votes there, with the addresses and highest
// levels of this board's command DB as a fallback.
//
STATIC CONST DSP_DESC  mDspDescs[] = {
  {
    "adsp",
    "qcom,sc7280-adsp-pas",
    1,
    2,
    443,
    429,
    (3 << 16) | 2,      // IPCC_CLIENT_LPASS, IPCC_MPROC_SIGNAL_SMP2P
    423,
    0x1800,
    0x0,
    {
      { "lcx.lvl", 0x30030, 6 },
      { "lmx.lvl", 0x30040, 6 },
    },
    {
      { NULL, 0, FALSE },
    },
  },
  {
    "cdsp",
    "qcom,sc7280-cdsp-pas",
    18,
    5,
    94,
    432,
    (6 << 16) | 2,      // IPCC_CLIENT_CDSP, IPCC_MPROC_SIGNAL_SMP2P
    601,
    0x11A0,
    0x400,
    {
      { "cx.lvl", 0x30000, 7 },
      { "mx.lvl", 0x30010, 10 },
    },
    //
    // The compute NoC part of the CDSP's path to DDR (nsp_noc
    // MASTER_CDSP_PROC to mc_virt SLAVE_EBI1): CO3 and CO0 share a voter,
    // SH3 has its own. SH0, MC0 and ACV are kept alive by apps anyway.
    //
    {
      { "CO3", 0x500A0, FALSE },
      { "CO0", 0x50094, TRUE  },
      { "SH3", 0x50010, TRUE  },
    },
  },
};

STATIC DSP                    mDsps[ARRAY_SIZE (mDspDescs)];
STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;
STATIC QCS6490_SMMU_PROTOCOL  *mSmmu;
STATIC BOOLEAN                mAtEl2;         // UEFI runs at EL2: Gunyah left in SEC
STATIC BOOLEAN                mDeferredExit;  // Gunyah leaves at ExitBootServices
STATIC BOOLEAN                mOsAtEl2;       // either: the OS runs at EL2
STATIC BOOLEAN                mShmBridge;
STATIC BOOLEAN                mShmBridgeTried;
STATIC BOOLEAN                mDone;
STATIC BOOLEAN                mXoVoted;
STATIC BOOLEAN                mPilInfoCleared;

/**
  Prints a line on the serial console, in RELEASE builds too, as SEC does
  for the exception level.

  @param[in]  Format  The format.
  @param[in]  ...     Its arguments.
**/
STATIC
VOID
EFIAPI
DspPrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[160];
  VA_LIST  Marker;
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  SerialPortWrite ((UINT8 *)Buffer, Length);
}

EFI_STATUS
DspSetMemoryAttributes (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size,
  IN UINT64                Attributes
  )
{
  return mCpu->SetMemoryAttributes (mCpu, Base, Size, Attributes);
}

/**
  Marks a DSP as failed.

  @param[in,out]  Dsp     The DSP.
  @param[in]      Reason  Why.
  @param[in]      Status  The error.
**/
STATIC
VOID
DspFail (
  IN OUT DSP          *Dsp,
  IN     CONST CHAR8  *Reason,
  IN     EFI_STATUS   Status
  )
{
  Dsp->State  = DspStateFailed;
  Dsp->Reason = Reason;
  Dsp->Status = Status;
  DEBUG ((DEBUG_ERROR, "DspPreload: %a: %a: %r\n", Dsp->Desc->Name, Reason, Status));
}

/**
  Milliseconds since a performance counter value.

  @param[in]  Start  The counter value.

  @return  The time elapsed.
**/
STATIC
UINT64
DspElapsedMs (
  IN UINT64  Start
  )
{
  return DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter () - Start), 1000000);
}

/**
  Reads the DspPreload setting, or its defaults.

  @param[out]  Config  The setting.
**/
STATIC
VOID
DspGetConfig (
  OUT QCS6490_DSP_PRELOAD_CONFIG  *Config
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Size   = sizeof (*Config);
  Status = gRT->GetVariable (
                  QCS6490_DSP_PRELOAD_VARIABLE,
                  &gQcs6490PlatformConfigGuid,
                  NULL,
                  &Size,
                  Config
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (*Config))) {
    Config->Mode = QCS6490_DSP_PRELOAD_AUTO;
    Config->Adsp = TRUE;
    Config->Cdsp = TRUE;
  }
}

/**
  Votes an RPMh resource.

  Votes are not read back first: reads of power rail and bus resources have
  never been seen to complete on this board, and a read that does not
  complete leaves a TCS busy, for UEFI and for Linux.

  @param[in,out]  Vote  The vote; Addr set.
  @param[in]      Data  The value.
  @param[in]      Wait  Wait for completion.

  @retval EFI_SUCCESS  Voted.
  @retval Other        From RpmhWrite().
**/
STATIC
EFI_STATUS
DspVote (
  IN OUT DSP_VOTE  *Vote,
  IN     UINT32    Data,
  IN     BOOLEAN   Wait
  )
{
  RPMH_CMD    Cmd;
  EFI_STATUS  Status;

  Cmd.Addr = Vote->Addr;
  Cmd.Data = Data;
  Cmd.Wait = Wait;
  Status   = RpmhWrite (&Cmd, 1);
  if (!EFI_ERROR (Status)) {
    Vote->Voted = TRUE;
  }

  DEBUG ((DEBUG_INFO, "DspPreload: RPMh 0x%05x <- 0x%x: %r\n", Vote->Addr, Data, Status));
  return Status;
}

/**
  Casts a DSP's proxy votes: its load state for AOP, its power rails at
  their highest level, and for the CDSP the NoC path to memory at its
  highest bandwidth (qcom_q6v5_prepare, qcom_pas_pds_enable).

  @param[in,out]  Dsp  The DSP.

  @retval EFI_SUCCESS  Voted.
  @retval Other        A rail could not be voted.
**/
STATIC
EFI_STATUS
DspProxyVote (
  IN OUT DSP  *Dsp
  )
{
  CONST DSP_DESC  *Desc;
  EFI_STATUS      Status;
  UINTN           Index;
  UINT32          Addr;
  UINT32          Level;

  Desc = Dsp->Desc;

  //
  // Linux does not start a DSP AOP did not hear about. The Radxa Q6A's
  // firmware starts them without telling AOP at all, so carry on, but say
  // so.
  //
  Status = QmpSendLoadState (Desc->Name, TRUE);
  if (!EFI_ERROR (Status)) {
    Dsp->LoadStateOn = TRUE;
  } else {
    DspPrint ("DspPreload: %a: AOP did not take the load state (%r), starting anyway\n", Desc->Name, Status);
  }

  for (Index = 0; Index < DSP_MAX_BCMS && Desc->Bcms[Index].Name != NULL; Index++) {
    if (EFI_ERROR (CmdDbLookup (Desc->Bcms[Index].Name, &Addr, NULL, NULL))) {
      DEBUG ((DEBUG_WARN, "DspPreload: %a not in the command DB, using 0x%x\n", Desc->Bcms[Index].Name, Desc->Bcms[Index].FallbackAddr));
      Addr = Desc->Bcms[Index].FallbackAddr;
    }

    Dsp->Bcms[Index].Addr = Addr;
  }

  //
  // A BCM vote counts once the last command of its voter commits; only the
  // committing commands wait (bcm-voter.c).
  //
  for (Index = 0; Index < DSP_MAX_BCMS && Desc->Bcms[Index].Name != NULL; Index++) {
    Status = DspVote (
               &Dsp->Bcms[Index],
               RPMH_BCM_CMD (Desc->Bcms[Index].Commit, TRUE, 0, RPMH_BCM_VOTE_MASK),
               Desc->Bcms[Index].Commit
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "DspPreload: %a: no bandwidth vote on %a\n", Desc->Name, Desc->Bcms[Index].Name));
    }
  }

  //
  // XO, once for all DSPs. Like the rails, it stays voted until Linux
  // votes it itself.
  //
  if (!mXoVoted) {
    DSP_VOTE  Xo;

    if (EFI_ERROR (CmdDbLookup (DSP_XO_RESOURCE, &Addr, NULL, NULL))) {
      Addr = DSP_XO_FALLBACK_ADDR;
    }

    ZeroMem (&Xo, sizeof (Xo));
    Xo.Addr  = Addr;
    mXoVoted = !EFI_ERROR (DspVote (&Xo, DSP_XO_ON, TRUE));
  }

  for (Index = 0; Index < DSP_MAX_RAILS; Index++) {
    if (EFI_ERROR (CmdDbRailMaxLevel (Desc->Rails[Index].Name, &Addr, &Level))) {
      DEBUG ((
        DEBUG_WARN,
        "DspPreload: %a not in the command DB, using 0x%x level %u\n",
        Desc->Rails[Index].Name,
        Desc->Rails[Index].FallbackAddr,
        Desc->Rails[Index].FallbackLevel
        ));
      Addr  = Desc->Rails[Index].FallbackAddr;
      Level = Desc->Rails[Index].FallbackLevel;
    }

    Dsp->Rails[Index].Addr = Addr;
    Status                 = DspVote (&Dsp->Rails[Index], Level, TRUE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Drops the bandwidth votes DspProxyVote() cast, as Linux does at handover
  (icc_set_bw (path, 0, 0)).

  The power rails stay voted at their highest level. Linux keeps every rail
  it manages at its highest level until its power domains sync, then votes
  each again, nothing for those no driver uses; lowering cx or mx here could
  take power from UFS or the display while UEFI still uses them.

  @param[in,out]  Dsp  The DSP.
**/
STATIC
VOID
DspProxyUnvote (
  IN OUT DSP  *Dsp
  )
{
  UINTN     Index;
  RPMH_CMD  Cmd;

  for (Index = 0; Index < DSP_MAX_BCMS && Dsp->Desc->Bcms[Index].Name != NULL; Index++) {
    if (!Dsp->Bcms[Index].Voted) {
      continue;
    }

    Cmd.Addr = Dsp->Bcms[Index].Addr;
    Cmd.Data = RPMH_BCM_CMD (Dsp->Desc->Bcms[Index].Commit, FALSE, 0, 0);
    Cmd.Wait = Dsp->Desc->Bcms[Index].Commit;
    RpmhWrite (&Cmd, 1);
    Dsp->Bcms[Index].Voted = FALSE;
  }
}

/**
  Undoes a start that went wrong after TrustZone took the image: stops the
  DSP, tells AOP and puts the votes back.

  @param[in,out]  Dsp  The DSP.
**/
STATIC
VOID
DspStop (
  IN OUT DSP  *Dsp
  )
{
  EFI_STATUS  Status;

  Status = ScmPasShutdown (Dsp->Desc->PasId);
  DEBUG ((DEBUG_INFO, "DspPreload: %a: PAS shutdown: %r\n", Dsp->Desc->Name, Status));

  //
  // A stopped DSP must not look started to Linux, which takes its SMP2P
  // entry as it finds it. If TrustZone did not stop it, nobody knows.
  //
  if (!EFI_ERROR (Status)) {
    SmemClearSmp2pInbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pItem);
  } else {
    DspPrint ("DspPreload: %a: TrustZone did not stop it (%r), its state is unknown\n", Dsp->Desc->Name, Status);
  }

  if (Dsp->LoadStateOn) {
    QmpSendLoadState (Dsp->Desc->Name, FALSE);
    Dsp->LoadStateOn = FALSE;
  }

  DspProxyUnvote (Dsp);
}

/**
  Reads and checks a DSP's firmware, and finds what the device tree says
  about it.

  @param[in,out]  Dsp  The DSP.

  @retval TRUE   Ready to start.
  @retval FALSE  Skipped or failed; Dsp says why.
**/
STATIC
BOOLEAN
DspPrepare (
  IN OUT DSP  *Dsp
  )
{
  EFI_STATUS  Status;
  UINT32      Bits;

  Status = DtGetDsp (Dsp);
  if (Status == EFI_NOT_FOUND) {
    Dsp->State  = DspStateSkipped;
    Dsp->Reason = "not in the device tree";
    return FALSE;
  }

  if (EFI_ERROR (Status)) {
    DspFail (Dsp, "unusable device tree node", Status);
    return FALSE;
  }

  //
  // The DSP is in reset, but SMEM may still hold its SMP2P entry from before
  // a warm reset, with ready and handover set. Clear it, so that only what
  // the DSP sets once started counts.
  //
  if (!EFI_ERROR (SmemReadSmp2pInbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pItem, &Bits))) {
    DEBUG ((DEBUG_WARN, "DspPreload: %a: SMP2P entry left from before, bits 0x%x, cleared\n", Dsp->Desc->Name, Bits));
    SmemClearSmp2pInbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pItem);
  }

  Status = FirmwareRead (Dsp->FirmwareName, &Dsp->Image.Data, &Dsp->Image.Size);
  if (EFI_ERROR (Status)) {
    DspFail (Dsp, "firmware not found", Status);
    return FALSE;
  }

  Status = MdtParse (&Dsp->Image);
  if (EFI_ERROR (Status)) {
    DspFail (Dsp, "not a usable firmware image", Status);
    MdtFree (&Dsp->Image);
    return FALSE;
  }

  if (Dsp->Image.Relocatable && (Dsp->Image.MaxAddr - Dsp->Image.MinAddr > Dsp->CarveoutSize)) {
    DspFail (Dsp, "image larger than its memory", EFI_BAD_BUFFER_SIZE);
    MdtFree (&Dsp->Image);
    return FALSE;
  }

  DEBUG ((
    DEBUG_INFO,
    "DspPreload: %a: %a, %lu bytes, into 0x%lx-0x%lx\n",
    Dsp->Desc->Name,
    Dsp->FirmwareName,
    (UINT64)Dsp->Image.Size,
    Dsp->CarveoutBase,
    Dsp->CarveoutBase + Dsp->CarveoutSize
    ));
  return TRUE;
}

/**
  Frees the metadata buffer DspInitImage() kept, and its SHM bridge.

  @param[in,out]  Dsp  The DSP.
**/
STATIC
VOID
DspReleaseMetadata (
  IN OUT DSP  *Dsp
  )
{
  if (Dsp->MetadataBridge != 0) {
    ScmShmBridgeDelete (Dsp->MetadataBridge);
    Dsp->MetadataBridge = 0;
  }

  if (Dsp->Metadata != 0) {
    DspSetMemoryAttributes (Dsp->Metadata, EFI_PAGES_TO_SIZE (Dsp->MetadataPages), EFI_MEMORY_WB | EFI_MEMORY_XP);
    gBS->FreePages (Dsp->Metadata, Dsp->MetadataPages);
    Dsp->Metadata = 0;
  }
}

/**
  Hands TrustZone the image metadata, in a buffer below 4 GiB, page aligned
  and uncached, as qcom_scm_pas_init_image() does. At EL2 a refused call is
  tried again with the buffer in an SHM bridge, in case TrustZone enforces
  bridges without a hypervisor to create them.

  Like Linux, the buffer is kept until the DSP has been started
  (DspReleaseMetadata()).

  @param[in,out]  Dsp  The DSP.

  @retval EFI_SUCCESS  TrustZone took the metadata.
  @retval Other        It did not; nothing is kept.
**/
STATIC
EFI_STATUS
DspInitImage (
  IN OUT DSP  *Dsp
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  EnableStatus;
  INT64       TzStatus;

  TzStatus = 0;

  Dsp->MetadataPages = EFI_SIZE_TO_PAGES (Dsp->Image.MetadataSize);
  Dsp->Metadata      = SIZE_4GB - 1;
  Status             = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Dsp->MetadataPages, &Dsp->Metadata);
  if (EFI_ERROR (Status)) {
    Dsp->Metadata = 0;
    return Status;
  }

  Status = DspSetMemoryAttributes (Dsp->Metadata, EFI_PAGES_TO_SIZE (Dsp->MetadataPages), EFI_MEMORY_WC | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    gBS->FreePages (Dsp->Metadata, Dsp->MetadataPages);
    Dsp->Metadata = 0;
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)Dsp->Metadata, EFI_PAGES_TO_SIZE (Dsp->MetadataPages));
  CopyMem ((VOID *)(UINTN)Dsp->Metadata, Dsp->Image.Metadata, Dsp->Image.MetadataSize);

  Status = EFI_DEVICE_ERROR;
  if (!mShmBridge) {
    Status = ScmPasInitImage (Dsp->Desc->PasId, Dsp->Metadata, &TzStatus);
    DEBUG ((DEBUG_INFO, "DspPreload: %a: PAS init_image: %r (%ld)\n", Dsp->Desc->Name, Status, TzStatus));
  }

  //
  // An error that says TrustZone does not support the call is final.
  //
  if (EFI_ERROR (Status) && (Status != EFI_UNSUPPORTED) && mAtEl2 && !mShmBridgeTried) {
    mShmBridgeTried = TRUE;
    EnableStatus    = ScmShmBridgeEnable ();
    DspPrint ("DspPreload: init_image refused (%ld), SHM bridge enable: %r\n", TzStatus, EnableStatus);
    mShmBridge = (EnableStatus != EFI_UNSUPPORTED);
  }

  if (EFI_ERROR (Status) && mShmBridge) {
    if (!EFI_ERROR (ScmShmBridgeCreate (Dsp->Metadata, EFI_PAGES_TO_SIZE (Dsp->MetadataPages), &Dsp->MetadataBridge))) {
      Status = ScmPasInitImage (Dsp->Desc->PasId, Dsp->Metadata, &TzStatus);
      DEBUG ((DEBUG_INFO, "DspPreload: %a: PAS init_image in an SHM bridge: %r (%ld)\n", Dsp->Desc->Name, Status, TzStatus));
    }
  }

  if (EFI_ERROR (Status)) {
    DspPrint ("DspPreload: %a: TrustZone refused the image (%ld)\n", Dsp->Desc->Name, TzStatus);
    DspReleaseMetadata (Dsp);
  }

  return Status;
}

/**
  Records where a DSP's image goes in the PIL relocation table in IMEM, for
  ramdump tools, as qcom_pil_info_store() does.

  @param[in]  Dsp  The DSP.
**/
STATIC
VOID
DspStoreRelocInfo (
  IN CONST DSP  *Dsp
  )
{
  UINTN   Entry;
  UINT32  Name[PIL_RELOC_NAME_LEN / sizeof (UINT32)];
  UINT32  Word;
  UINTN   Index;

  //
  // The table holds whatever the last boot left there; Linux clears it
  // before its first entry (qcom_pil_info_init), and so does this.
  //
  if (!mPilInfoCleared) {
    for (Entry = PIL_RELOC_BASE; Entry < PIL_RELOC_BASE + PIL_RELOC_SIZE; Entry += sizeof (UINT32)) {
      MmioWrite32 (Entry, 0);
    }

    mPilInfoCleared = TRUE;
  }

  ZeroMem (Name, sizeof (Name));
  CopyMem (Name, Dsp->Desc->Name, MIN (AsciiStrLen (Dsp->Desc->Name), PIL_RELOC_NAME_LEN));

  for (Entry = PIL_RELOC_BASE; Entry + PIL_RELOC_ENTRY_SIZE <= PIL_RELOC_BASE + PIL_RELOC_SIZE; Entry += PIL_RELOC_ENTRY_SIZE) {
    Word = MmioRead32 (Entry);
    if (((Word & 0xFF) != 0) && ((Word != Name[0]) || (MmioRead32 (Entry + 4) != Name[1]))) {
      continue;
    }

    for (Index = 0; Index < ARRAY_SIZE (Name); Index++) {
      MmioWrite32 (Entry + 4 * Index, Name[Index]);
    }

    MmioWrite32 (Entry + 8, (UINT32)Dsp->CarveoutBase);
    MmioWrite32 (Entry + 12, (UINT32)RShiftU64 (Dsp->CarveoutBase, 32));
    MmioWrite32 (Entry + 16, (UINT32)Dsp->CarveoutSize);
    return;
  }

  DEBUG ((DEBUG_WARN, "DspPreload: no room for %a in the PIL relocation table\n", Dsp->Desc->Name));
}

/**
  Starts a prepared DSP: hands its SMMU stream over, votes, loads it and has
  TrustZone authenticate and release it.

  @param[in,out]  Dsp  The DSP.
**/
STATIC
VOID
DspStart (
  IN OUT DSP  *Dsp
  )
{
  EFI_STATUS  Status;
  INT64       TzStatus;
  UINT64      Bridge;
  VOID        *Region;

  if (mAtEl2) {
    Status = mSmmu->HandOverBypass (mSmmu, Dsp->Desc->StreamId, Dsp->Desc->StreamMask, Dsp->Desc->Name);
    if (EFI_ERROR (Status)) {
      DspFail (Dsp, "SMMU stream not handed over", Status);
      goto FreeImage;
    }
  }

  //
  // Linux's SMP2P driver creates apps' entries for each DSP when it starts,
  // long before it starts a DSP; the DSP may wait for them.
  //
  Status = SmemCreateSmp2pOutbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pOutItem, Dsp->Desc->Smp2pItem, Dsp->Desc->Smp2pIpcc);
  if (EFI_ERROR (Status)) {
    DspPrint ("DspPreload: %a: apps SMP2P entries not created (%r)\n", Dsp->Desc->Name, Status);
  }

  Status = DspProxyVote (Dsp);
  if (EFI_ERROR (Status)) {
    DspFail (Dsp, "power rails not voted", Status);
    goto Unvote;
  }

  if (!ScmPasIsSupported (Dsp->Desc->PasId)) {
    DEBUG ((DEBUG_WARN, "DspPreload: %a: TrustZone does not say it supports PAS ID %u\n", Dsp->Desc->Name, Dsp->Desc->PasId));
  }

  Status = DspInitImage (Dsp);
  if (EFI_ERROR (Status)) {
    DspFail (Dsp, "TrustZone refused the metadata", Status);
    goto Unvote;
  }

  if (Dsp->Image.Relocatable) {
    Status = ScmPasMemSetup (Dsp->Desc->PasId, Dsp->CarveoutBase, Dsp->Image.MaxAddr - Dsp->Image.MinAddr, &TzStatus);
    DEBUG ((DEBUG_INFO, "DspPreload: %a: PAS mem_setup: %r (%ld)\n", Dsp->Desc->Name, Status, TzStatus));
    if (EFI_ERROR (Status)) {
      DspFail (Dsp, "TrustZone refused the memory", Status);
      goto Stop;
    }
  }

  //
  // The carve-out is not in UEFI's page tables. Write it uncached, so that
  // no line of it lingers in a cache once TrustZone has locked it.
  //
  Status = DspSetMemoryAttributes (Dsp->CarveoutBase, Dsp->CarveoutSize, EFI_MEMORY_WC | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    DspFail (Dsp, "carve-out not mapped", Status);
    goto Stop;
  }

  Region = (VOID *)(UINTN)Dsp->CarveoutBase;
  Status = MdtLoadSegments (&Dsp->Image, Region, Dsp->CarveoutBase, Dsp->CarveoutSize);
  ArmDataSynchronizationBarrier ();
  if (EFI_ERROR (Status)) {
    DspSetMemoryAttributes (Dsp->CarveoutBase, Dsp->CarveoutSize, EFI_MEMORY_WC | EFI_MEMORY_XP | EFI_MEMORY_RP);
    DspFail (Dsp, "segments do not fit", Status);
    goto Stop;
  }

  //
  // Linux records where the image went before it starts it.
  //
  DspStoreRelocInfo (Dsp);

  //
  // Linux at EL2 puts the carve-out in an SHM bridge around the call
  // (qcom_scm_pas_prepare_and_auth_reset).
  //
  Bridge = 0;
  if (mShmBridge && EFI_ERROR (ScmShmBridgeCreate (Dsp->CarveoutBase, Dsp->CarveoutSize, &Bridge))) {
    Bridge = 0;
  }

  Status = ScmPasAuthAndReset (Dsp->Desc->PasId, &TzStatus);
  Dsp->StartedAt = GetPerformanceCounter ();

  if (Bridge != 0) {
    ScmShmBridgeDelete (Bridge);
  }

  DspReleaseMetadata (Dsp);

  //
  // The carve-out belongs to the DSP from now on; keep the CPU out of it.
  //
  DspSetMemoryAttributes (Dsp->CarveoutBase, Dsp->CarveoutSize, EFI_MEMORY_WC | EFI_MEMORY_XP | EFI_MEMORY_RP);

  DEBUG ((DEBUG_INFO, "DspPreload: %a: PAS auth_and_reset: %r (%ld)\n", Dsp->Desc->Name, Status, TzStatus));
  if (EFI_ERROR (Status)) {
    DspPrint ("DspPreload: %a: TrustZone did not start the DSP (%ld)\n", Dsp->Desc->Name, TzStatus);
    DspFail (Dsp, "TrustZone did not start it", Status);
    goto Stop;
  }

  Dsp->State = DspStateStarted;
  goto FreeImage;

Stop:
  DspStop (Dsp);
  DspReleaseMetadata (Dsp);
  goto FreeImage;

Unvote:
  if (Dsp->LoadStateOn) {
    QmpSendLoadState (Dsp->Desc->Name, FALSE);
    Dsp->LoadStateOn = FALSE;
  }

  DspProxyUnvote (Dsp);

FreeImage:
  MdtFree (&Dsp->Image);
}

/**
  Logs why a DSP crashed, from its crash reason in SMEM.

  @param[in]  Dsp  The DSP.
**/
STATIC
VOID
DspLogCrashReason (
  IN CONST DSP  *Dsp
  )
{
  VOID         *Data;
  UINTN        Size;
  CHAR8        Reason[128];
  UINTN        Index;
  CONST CHAR8  *Text;

  if (EFI_ERROR (SmemGetGlobalItem (Dsp->Desc->CrashReasonItem, &Data, &Size)) || (Size == 0)) {
    DspPrint ("DspPreload: %a: crashed, no crash reason\n", Dsp->Desc->Name);
    return;
  }

  Text = Data;
  for (Index = 0; Index < MIN (Size, sizeof (Reason) - 1) && Text[Index] != '\0'; Index++) {
    Reason[Index] = ((Text[Index] >= 0x20) && (Text[Index] < 0x7F)) ? Text[Index] : '.';
  }

  Reason[Index] = '\0';
  DspPrint ("DspPreload: %a: crashed: %a\n", Dsp->Desc->Name, Reason);
}

/**
  Returns whether SMP2P bits say what Linux takes for a running DSP it can
  attach to: ready and handover, no stop acknowledged, no error
  (qcom_pas_get_boot_state_from_smp2p).

  @param[in]  Bits  The bits.

  @return  TRUE if they do.
**/
STATIC
BOOLEAN
DspBitsRunning (
  IN UINT32  Bits
  )
{
  return (Bits & (SMP2P_BIT_FATAL | SMP2P_BIT_READY | SMP2P_BIT_HANDOVER | SMP2P_BIT_STOP_ACK)) ==
         (SMP2P_BIT_READY | SMP2P_BIT_HANDOVER);
}

/**
  Logs the apps SMMU's global fault state, at EL2 only (under Gunyah the
  registers belong to the hypervisor). A DSP whose memory accesses the SMMU
  blocks shows up here.
**/
STATIC
VOID
DspLogSmmuFaults (
  VOID
  )
{
  if (!mAtEl2) {
    return;
  }

  DspPrint (
    "DspPreload: SMMU sCR0 0x%x sGFSR 0x%x sGFSYNR0 0x%x sGFSYNR1 0x%x sGFSYNR2 0x%x\n",
    MmioRead32 (DSP_APPS_SMMU_BASE + 0x000),
    MmioRead32 (DSP_APPS_SMMU_BASE + 0x048),
    MmioRead32 (DSP_APPS_SMMU_BASE + 0x050),
    MmioRead32 (DSP_APPS_SMMU_BASE + 0x054),
    MmioRead32 (DSP_APPS_SMMU_BASE + 0x058)
    );
}

/**
  Marks a started DSP as running: drops its bandwidth votes and records it
  in IMEM.

  @param[in,out]  Dsp  The DSP.
**/
STATIC
VOID
DspRunning (
  IN OUT DSP  *Dsp
  )
{
  EFI_STATUS  Status;

  Dsp->State = DspStateRunning;
  DspProxyUnvote (Dsp);
  DspPrint ("DspPreload: %a running after %lu ms\n", Dsp->Desc->Name, DspElapsedMs (Dsp->StartedAt));

  //
  // Under Gunyah, SmmuDxe records the stream and sets it up once Gunyah has
  // left.
  //
  if (mDeferredExit) {
    Status = mSmmu->HandOverBypass (mSmmu, Dsp->Desc->StreamId, Dsp->Desc->StreamMask, Dsp->Desc->Name);
    if (EFI_ERROR (Status)) {
      DspPrint ("DspPreload: %a: SMMU stream not recorded for the OS: %r\n", Dsp->Desc->Name, Status);
    }
  }
}

/**
  Waits for the started DSPs to say ready and to release their proxy votes
  (handover), as Linux waits for ready and then for the handover interrupt.
**/
STATIC
VOID
DspWaitForDsps (
  VOID
  )
{
  UINTN       Index;
  UINTN       Waiting;
  DSP         *Dsp;
  EFI_STATUS  Status;
  UINT32      Bits;

  do {
    Waiting = 0;
    for (Index = 0; Index < ARRAY_SIZE (mDsps); Index++) {
      Dsp = &mDsps[Index];
      if (Dsp->State != DspStateStarted) {
        continue;
      }

      SmemNegotiateSmp2p (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pOutItem, Dsp->Desc->Smp2pItem, Dsp->Desc->Smp2pIpcc);

      Status = SmemReadSmp2pInbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pItem, &Bits);
      if (!EFI_ERROR (Status)) {
        if (Bits != Dsp->Smp2pBits) {
          DEBUG ((DEBUG_INFO, "DspPreload: %a: SMP2P 0x%x after %lu ms\n", Dsp->Desc->Name, Bits, DspElapsedMs (Dsp->StartedAt)));
        }

        Dsp->Smp2pBits = Bits;

        if ((Bits & SMP2P_BIT_FATAL) != 0) {
          DspLogCrashReason (Dsp);
          DspFail (Dsp, "crashed while starting", EFI_DEVICE_ERROR);
          DspStop (Dsp);
          continue;
        }

        if (DspBitsRunning (Bits)) {
          DspRunning (Dsp);
          continue;
        }
      }

      if (DspElapsedMs (Dsp->StartedAt) > DSP_START_TIMEOUT_MS) {
        //
        // One last look, so that a DSP that got there as time ran out is
        // not stopped while it says it runs.
        //
        Status = SmemReadSmp2pInbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pItem, &Bits);
        if (!EFI_ERROR (Status) && DspBitsRunning (Bits)) {
          DspRunning (Dsp);
          continue;
        }

        DspPrint (
          "DspPreload: %a: no ready and handover after %u ms (SMP2P %a 0x%x)\n",
          Dsp->Desc->Name,
          DSP_START_TIMEOUT_MS,
          (Status == EFI_NOT_FOUND) ? "item missing," : ((Status == EFI_NOT_READY) ? "no entries yet," : "bits"),
          EFI_ERROR (Status) ? 0 : Bits
          );
        DspLogSmmuFaults ();
        DspLogCrashReason (Dsp);
        SmemLogPartition (Dsp->Desc->SmemHost);
        DspFail (Dsp, "did not start", EFI_TIMEOUT);
        DspStop (Dsp);
        continue;
      }

      Waiting++;
    }

    if (Waiting != 0) {
      MicroSecondDelay (DSP_POLL_INTERVAL_US);
    }
  } while (Waiting != 0);
}

/**
  Logs the SMP2P state of the DSPs that run, around the late Gunyah exit:
  once before, and after it for a moment, to tell whether they survive it.
  Runs with no boot services (QCS6490_GUNYAH_EXIT_NOTIFY).

  @param[in]  Context  Unused.
  @param[in]  Phase    Where the exit is.
**/
STATIC
VOID
EFIAPI
DspOnGunyahExit (
  IN VOID                       *Context,
  IN QCS6490_GUNYAH_EXIT_PHASE  Phase
  )
{
  UINTN        Index;
  UINTN        Elapsed;
  DSP          *Dsp;
  UINT32       Bits;
  EFI_STATUS   Status;
  CONST CHAR8  *When;

  When = (Phase == Qcs6490GunyahExitBefore) ? "before" : "after";

  for (Index = 0; Index < ARRAY_SIZE (mDsps); Index++) {
    Dsp = &mDsps[Index];
    if (Dsp->State != DspStateRunning) {
      continue;
    }

    //
    // After the exit, watch for 200 ms: a DSP that loses its memory or its
    // stream faults soon.
    //
    Elapsed = 0;
    do {
      Status = SmemReadSmp2pInbound (Dsp->Desc->SmemHost, Dsp->Desc->Smp2pItem, &Bits);
      if (EFI_ERROR (Status) || ((Bits & SMP2P_BIT_FATAL) != 0) || (Phase == Qcs6490GunyahExitBefore)) {
        break;
      }

      MicroSecondDelay (10000);
      Elapsed += 10;
    } while (Elapsed < 200);

    if (EFI_ERROR (Status)) {
      DspPrint ("DspPreload: %a the Gunyah exit: %a SMP2P not readable (%r)\n", When, Dsp->Desc->Name, Status);
      continue;
    }

    DspPrint (
      "DspPreload: %a the Gunyah exit: %a SMP2P 0x%x (%a)\n",
      When,
      Dsp->Desc->Name,
      Bits,
      ((Bits & SMP2P_BIT_FATAL) != 0) ? "crashed" : (DspBitsRunning (Bits) ? "running" : "not ready")
      );
    if ((Bits & SMP2P_BIT_FATAL) != 0) {
      DspLogCrashReason (Dsp);
    }
  }
}

/**
  Has the DSPs that run watched around the late Gunyah exit.
**/
STATIC
VOID
DspWatchGunyahExit (
  VOID
  )
{
  QCS6490_GUNYAH_EXIT_PROTOCOL  *GunyahExit;
  UINTN                         Index;

  for (Index = 0; Index < ARRAY_SIZE (mDsps); Index++) {
    if (mDsps[Index].State == DspStateRunning) {
      break;
    }
  }

  if ((Index == ARRAY_SIZE (mDsps)) ||
      EFI_ERROR (gBS->LocateProtocol (&gQcs6490GunyahExitProtocolGuid, NULL, (VOID **)&GunyahExit)))
  {
    return;
  }

  GunyahExit->RegisterNotify (GunyahExit, DspOnGunyahExit, NULL);
}

/**
  Preloads the DSPs the setting asks for.
**/
STATIC
VOID
DspPreloadAll (
  VOID
  )
{
  QCS6490_DSP_PRELOAD_CONFIG  Config;
  EFI_STATUS                  Status;
  EFI_PHYSICAL_ADDRESS        CmdDbBase;
  UINT64                      CmdDbSize;
  UINTN                       Index;
  UINTN                       Prepared;
  VOID                        *Fdt;

  DspGetConfig (&Config);
  mAtEl2        = (ArmReadCurrentEL () == AARCH64_EL2);
  mDeferredExit = Qcs6490GunyahExitDeferred ();
  mOsAtEl2      = mAtEl2 || mDeferredExit;

  if (Config.Mode == QCS6490_DSP_PRELOAD_DISABLED) {
    DEBUG ((DEBUG_INFO, "DspPreload: disabled\n"));
    return;
  }

  if ((Config.Mode != QCS6490_DSP_PRELOAD_ALWAYS) && !mOsAtEl2) {
    DEBUG ((DEBUG_INFO, "DspPreload: at EL1 the OS starts the DSPs itself\n"));
    return;
  }

  //
  // Gunyah has gone already (SEC left it, e.g. with PcdGunyahLateExit 0):
  // TrustZone would accept the PAS calls, but the DSPs would not run.
  //
  if ((Config.Mode != QCS6490_DSP_PRELOAD_ALWAYS) && mAtEl2) {
    DspPrint ("DspPreload: Gunyah left before the DSPs could be started, skipped (Always forces it)\n");
    return;
  }

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status)) {
    DspPrint ("DspPreload: no CPU protocol\n");
    return;
  }

  if (mOsAtEl2) {
    Status = gBS->LocateProtocol (&gQcs6490SmmuProtocolGuid, NULL, (VOID **)&mSmmu);
    if (EFI_ERROR (Status)) {
      DspPrint ("DspPreload: no SMMU protocol, the DSPs' streams cannot be handed over\n");
      return;
    }
  }

  Status = SmemInit ();
  if (EFI_ERROR (Status)) {
    DspPrint ("DspPreload: SMEM not usable (%r)\n", Status);
    return;
  }

  if (EFI_ERROR (DtGetCmdDb (&CmdDbBase, &CmdDbSize)) || EFI_ERROR (CmdDbInit (CmdDbBase, CmdDbSize))) {
    DEBUG ((DEBUG_WARN, "DspPreload: no command DB, using this board's addresses\n"));
  }

  DspPrint (
    "DspPreload: starting the DSPs at EL%u%a\n",
    mAtEl2 ? 2 : 1,
    mDeferredExit ? ", for an OS at EL2 (Gunyah leaves at ExitBootServices)" : ""
    );

  Prepared = 0;
  for (Index = 0; Index < ARRAY_SIZE (mDsps); Index++) {
    mDsps[Index].Desc  = &mDspDescs[Index];
    mDsps[Index].State = DspStateIdle;

    if (((Index == 0) && !Config.Adsp) || ((Index == 1) && !Config.Cdsp)) {
      mDsps[Index].State  = DspStateSkipped;
      mDsps[Index].Reason = "disabled in the settings";
      continue;
    }

    if (DspPrepare (&mDsps[Index])) {
      Prepared++;
    }
  }

  DEBUG ((DEBUG_INFO, "DspPreload: %u DSP(s) to start\n", (UINT32)Prepared));

  for (Index = 0; Index < ARRAY_SIZE (mDsps); Index++) {
    if ((mDsps[Index].State == DspStateIdle) && (mDsps[Index].Image.Data != NULL)) {
      DspStart (&mDsps[Index]);
    }
  }

  DspWaitForDsps ();

  for (Index = 0; Index < ARRAY_SIZE (mDsps); Index++) {
    switch (mDsps[Index].State) {
      case DspStateRunning:
        break;
      case DspStateSkipped:
        DEBUG ((DEBUG_INFO, "DspPreload: %a skipped: %a\n", mDspDescs[Index].Name, mDsps[Index].Reason));
        break;
      default:
        DspPrint (
          "DspPreload: %a not started: %a (%r)\n",
          mDspDescs[Index].Name,
          (mDsps[Index].Reason != NULL) ? mDsps[Index].Reason : "unknown",
          mDsps[Index].Status
          );
        break;
    }
  }

  if (mDeferredExit) {
    DspWatchGunyahExit ();
  }

  //
  // The board's own device tree, for an OS loaded without a device tree of
  // its own. GRUB's goes through EFI_DT_FIXUP_PROTOCOL.
  //
  if (!EFI_ERROR (EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt)) && (Fdt != NULL)) {
    DtFixupRunningDsps (Fdt, mDsps, ARRAY_SIZE (mDsps));
  }

  RpmhLogState ("DspPreload done");
}

/**
  Preloads the DSPs when the boot manager is about to boot, once.

  @param[in]  Event    The ReadyToBoot event.
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
  if (mDone) {
    return;
  }

  mDone = TRUE;
  gBS->CloseEvent (Event);

  DspPreloadAll ();
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The preload is set up for ReadyToBoot.
  @retval Other        It could not be.
**/
EFI_STATUS
EFIAPI
DspPreloadDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Status = DtInstallFixupProtocol (mDsps, ARRAY_SIZE (mDsps));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                OnReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
