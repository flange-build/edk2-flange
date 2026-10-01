/** @file
  Lets drivers hand an apps SMMU stream over to the OS in bypass, for a bus
  master that keeps running after ExitBootServices, such as a remote
  processor UEFI started.

  Installed by SmmuDxe when UEFI runs at EL2. Under Gunyah the hypervisor
  owns the SMMU and sets up the streams of the remote processors itself, so
  there is nothing to hand over.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_SMMU_H_
#define QCS6490_SMMU_H_

#define QCS6490_SMMU_PROTOCOL_GUID \
  { 0xe7c57892, 0x299f, 0x4197, { 0xa2, 0xb0, 0x7b, 0xee, 0x2c, 0x46, 0xa4, 0x5d } }

typedef struct _QCS6490_SMMU_PROTOCOL QCS6490_SMMU_PROTOCOL;

/**
  Makes a stream bypass the SMMU, now and for the OS: a valid stream match
  entry in the format without extended IDs, with a BYPASS stream-to-context
  entry, which stays at ExitBootServices. Linux (arm-smmu-qcom
  qcom_smmu_cfg_probe) adopts the stream match entries it finds valid as
  bypass streams that no device owns.

  @param[in]  This      The protocol.
  @param[in]  StreamId  The stream ID.
  @param[in]  Mask      The stream ID bits to ignore.
  @param[in]  Name      A name for the log.

  @retval EFI_SUCCESS            The stream bypasses the SMMU, now and for the
                                 OS.
  @retval EFI_ACCESS_DENIED      Another entry already matches the stream and
                                 does not bypass.
  @retval EFI_UNSUPPORTED        The SMMU uses extended stream IDs, which
                                 Linux does not adopt.
  @retval EFI_OUT_OF_RESOURCES   No stream match entry is free.
**/
typedef
EFI_STATUS
(EFIAPI *QCS6490_SMMU_HAND_OVER_BYPASS)(
  IN QCS6490_SMMU_PROTOCOL  *This,
  IN UINT16                 StreamId,
  IN UINT16                 Mask,
  IN CONST CHAR8            *Name
  );

struct _QCS6490_SMMU_PROTOCOL {
  QCS6490_SMMU_HAND_OVER_BYPASS    HandOverBypass;
};

extern EFI_GUID  gQcs6490SmmuProtocolGuid;

#endif // QCS6490_SMMU_H_
