/** @file
  Lets drivers act around the late Gunyah exit: in a boot that runs the OS
  at EL2 but leaves Gunyah only once the OS loader's ExitBootServices
  succeeded (Library/Qcs6490NvStatusLib.h), GunyahExitDxe calls them right
  before and right after it asks for EL2.

  Installed by GunyahExitDxe in such boots only.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_GUNYAH_EXIT_H_
#define QCS6490_GUNYAH_EXIT_H_

#define QCS6490_GUNYAH_EXIT_PROTOCOL_GUID \
  { 0x4c6bcf46, 0x5e19, 0x4af9, { 0x83, 0xcd, 0x92, 0x67, 0x3c, 0x26, 0xb6, 0x75 } }

typedef struct _QCS6490_GUNYAH_EXIT_PROTOCOL QCS6490_GUNYAH_EXIT_PROTOCOL;

typedef enum {
  Qcs6490GunyahExitBefore,      // still a Gunyah guest at EL1, about to ask
  Qcs6490GunyahExitDone,        // at EL2, the MMU back on
  Qcs6490GunyahExitRefused      // Gunyah stays: still at EL1
} QCS6490_GUNYAH_EXIT_PHASE;

/**
  Called around the exit, after the original ExitBootServices returned: no
  boot services, interrupts masked. Only MMIO, memory UEFI mapped before,
  SerialPortLib, TimerLib and fixed PCDs may be used.

  @param[in]  Context  What the driver registered.
  @param[in]  Phase    Where the exit is.
**/
typedef
VOID
(EFIAPI *QCS6490_GUNYAH_EXIT_NOTIFY)(
  IN VOID                       *Context,
  IN QCS6490_GUNYAH_EXIT_PHASE  Phase
  );

/**
  Registers a function to call around the exit.

  @param[in]  This     The protocol.
  @param[in]  Notify   The function.
  @param[in]  Context  Passed to it.

  @retval EFI_SUCCESS           Registered.
  @retval EFI_OUT_OF_RESOURCES  Too many functions registered.
**/
typedef
EFI_STATUS
(EFIAPI *QCS6490_GUNYAH_EXIT_REGISTER_NOTIFY)(
  IN QCS6490_GUNYAH_EXIT_PROTOCOL  *This,
  IN QCS6490_GUNYAH_EXIT_NOTIFY    Notify,
  IN VOID                          *Context
  );

struct _QCS6490_GUNYAH_EXIT_PROTOCOL {
  QCS6490_GUNYAH_EXIT_REGISTER_NOTIFY    RegisterNotify;
};

extern EFI_GUID  gQcs6490GunyahExitProtocolGuid;

#endif // QCS6490_GUNYAH_EXIT_H_
