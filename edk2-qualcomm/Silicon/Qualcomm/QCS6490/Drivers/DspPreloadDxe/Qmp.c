/** @file
  Messages to AOP (the always-on processor) through the AOSS QMP mailbox, as
  Linux's drivers/soc/qcom/qcom_aoss.c sends them: before Linux starts a
  remote processor it tells AOP that its image is loaded.

  The QMP message RAM starts with a descriptor of two link and channel
  states, one per side, which each side acknowledges; then each side has a
  mailbox. A message is written after a length word, the length last, and
  AOP clears the length once it has taken the message. Each step is
  signalled to AOP through the IPCC doorbell.

  The link is left open: Linux's qcom_aoss opens it again at probe, which
  goes straight through when the states are already up.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/PrintLib.h>

#include "DspPreload.h"

//
// aoss_qmp and ipcc in kodiak.dtsi. The QMP mailbox of AOP is IPCC client 0
// (IPCC_CLIENT_AOP), signal 0 (IPCC_MPROC_SIGNAL_GLINK_QMP).
//
#define QMP_MSGRAM_BASE      0x0C300000
#define QMP_MSGRAM_SIZE      0x400
#define IPCC_BASE            0x00408000
#define IPCC_REG_SEND_ID     0x0C
#define IPCC_AOP_GLINK_QMP   ((0U << 16) | 0)

//
// Descriptor at the start of the message RAM (qcom_aoss.c).
//
#define QMP_DESC_MAGIC                  0x00
#define QMP_DESC_VERSION                0x04
#define QMP_DESC_UCORE_LINK_STATE       0x0C
#define QMP_DESC_UCORE_LINK_STATE_ACK   0x10
#define QMP_DESC_UCORE_CH_STATE         0x14
#define QMP_DESC_UCORE_CH_STATE_ACK     0x18
#define QMP_DESC_MCORE_LINK_STATE       0x24
#define QMP_DESC_MCORE_LINK_STATE_ACK   0x28
#define QMP_DESC_MCORE_CH_STATE         0x2C
#define QMP_DESC_MCORE_CH_STATE_ACK     0x30
#define QMP_DESC_MCORE_MBOX_SIZE        0x34
#define QMP_DESC_MCORE_MBOX_OFFSET      0x38

#define QMP_STATE_UP    0x0000FFFF
#define QMP_STATE_DOWN  0xFFFF0000
#define QMP_MAGIC       0x4D41494C    // "mail"
#define QMP_VERSION     1

//
// Linux sends every message in a 64-byte buffer and waits a second for AOP.
//
#define QMP_MSG_LEN     64
#define QMP_TIMEOUT_US  1000000

STATIC BOOLEAN  mQmpOpen;
STATIC BOOLEAN  mQmpFailed;
STATIC UINT32   mQmpOffset;

/**
  Rings AOP's doorbell.
**/
STATIC
VOID
QmpKick (
  VOID
  )
{
  MmioWrite32 (IPCC_BASE + IPCC_REG_SEND_ID, IPCC_AOP_GLINK_QMP);
}

/**
  Waits until a message RAM word reads a value.

  @param[in]  Offset  The word.
  @param[in]  Value   The value.

  @retval EFI_SUCCESS  It does.
  @retval EFI_TIMEOUT  It does not within a second.
**/
STATIC
EFI_STATUS
QmpWait (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  UINTN  Elapsed;

  for (Elapsed = 0; Elapsed < QMP_TIMEOUT_US; Elapsed += 10) {
    if (MmioRead32 (QMP_MSGRAM_BASE + Offset) == Value) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

/**
  Opens the link and the channel to AOP (qmp_open).

  @retval EFI_SUCCESS  Open.
  @retval Other        AOP's mailbox is not there or did not answer.
**/
STATIC
EFI_STATUS
QmpOpen (
  VOID
  )
{
  UINT32      Size;
  EFI_STATUS  Status;

  if (mQmpOpen) {
    return EFI_SUCCESS;
  }

  //
  // Each attempt can wait seconds for AOP; one failure is enough.
  //
  if (mQmpFailed) {
    return EFI_NOT_READY;
  }

  mQmpFailed = TRUE;

  if ((MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MAGIC) != QMP_MAGIC) ||
      (MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_VERSION) != QMP_VERSION))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no QMP descriptor (magic 0x%x, version %u)\n",
      __func__,
      MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MAGIC),
      MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_VERSION)
      ));
    return EFI_NOT_FOUND;
  }

  mQmpOffset = MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_MBOX_OFFSET);
  Size       = MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_MBOX_SIZE);
  if ((Size < sizeof (UINT32) + QMP_MSG_LEN) || ((UINT64)mQmpOffset + Size > QMP_MSGRAM_SIZE) ||
      ((mQmpOffset & 3) != 0))
  {
    DEBUG ((DEBUG_ERROR, "%a: bad mailbox 0x%x, %u bytes\n", __func__, mQmpOffset, Size));
    return EFI_NOT_FOUND;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: link %a/%a, channel %a/%a\n",
    __func__,
    (MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_LINK_STATE) == QMP_STATE_UP) ? "up" : "down",
    (MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_LINK_STATE_ACK) == QMP_STATE_UP) ? "acked" : "not acked",
    (MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_CH_STATE) == QMP_STATE_UP) ? "up" : "down",
    (MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_CH_STATE_ACK) == QMP_STATE_UP) ? "acked" : "not acked"
    ));

  //
  // Acknowledge AOP's link state, bring ours up, and wait for AOP to
  // acknowledge it.
  //
  MmioWrite32 (QMP_MSGRAM_BASE + QMP_DESC_UCORE_LINK_STATE_ACK, MmioRead32 (QMP_MSGRAM_BASE + QMP_DESC_UCORE_LINK_STATE));
  MmioWrite32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_LINK_STATE, QMP_STATE_UP);
  QmpKick ();
  Status = QmpWait (QMP_DESC_MCORE_LINK_STATE_ACK, QMP_STATE_UP);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: AOP did not acknowledge the link\n", __func__));
    goto CloseLink;
  }

  //
  // Our channel up, AOP's channel up, then acknowledge AOP's.
  //
  MmioWrite32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_CH_STATE, QMP_STATE_UP);
  QmpKick ();
  Status = QmpWait (QMP_DESC_UCORE_CH_STATE, QMP_STATE_UP);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: AOP did not open its channel\n", __func__));
    goto CloseChannel;
  }

  MmioWrite32 (QMP_MSGRAM_BASE + QMP_DESC_UCORE_CH_STATE_ACK, QMP_STATE_UP);
  QmpKick ();
  Status = QmpWait (QMP_DESC_MCORE_CH_STATE_ACK, QMP_STATE_UP);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: AOP did not acknowledge the channel\n", __func__));
    goto CloseChannel;
  }

  mQmpOpen   = TRUE;
  mQmpFailed = FALSE;
  return EFI_SUCCESS;

CloseChannel:
  MmioWrite32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_CH_STATE, QMP_STATE_DOWN);

CloseLink:
  MmioWrite32 (QMP_MSGRAM_BASE + QMP_DESC_MCORE_LINK_STATE, QMP_STATE_DOWN);
  QmpKick ();
  return Status;
}

/**
  Sends a message and waits until AOP has taken it (qmp_send).

  @param[in]  Message  The message, shorter than QMP_MSG_LEN.

  @retval EFI_SUCCESS  Taken.
  @retval Other        Not.
**/
STATIC
EFI_STATUS
QmpSend (
  IN CONST CHAR8  *Message
  )
{
  UINT32      Buffer[QMP_MSG_LEN / sizeof (UINT32)];
  UINTN       Index;
  EFI_STATUS  Status;

  Status = QmpOpen ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (Buffer, sizeof (Buffer));
  AsciiStrCpyS ((CHAR8 *)Buffer, sizeof (Buffer), Message);

  //
  // The message RAM only takes 32-bit accesses. The length goes last; it is
  // read back so that the message is there before the doorbell.
  //
  for (Index = 0; Index < ARRAY_SIZE (Buffer); Index++) {
    MmioWrite32 (QMP_MSGRAM_BASE + mQmpOffset + sizeof (UINT32) * (Index + 1), Buffer[Index]);
  }

  MmioWrite32 (QMP_MSGRAM_BASE + mQmpOffset, sizeof (Buffer));
  MmioRead32 (QMP_MSGRAM_BASE + mQmpOffset);
  QmpKick ();

  Status = QmpWait (mQmpOffset, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: AOP did not take \"%a\"\n", __func__, Message));
    MmioWrite32 (QMP_MSGRAM_BASE + mQmpOffset, 0);
  }

  return Status;
}

EFI_STATUS
QmpSendLoadState (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      On
  )
{
  CHAR8  Message[QMP_MSG_LEN];

  //
  // The message of qcom_q6v5.c q6v5_load_state_toggle().
  //
  AsciiSPrint (
    Message,
    sizeof (Message),
    "{class: image, res: load_state, name: %a, val: %a}",
    Name,
    On ? "on" : "off"
    );

  DEBUG ((DEBUG_INFO, "%a: %a\n", __func__, Message));
  return QmpSend (Message);
}
