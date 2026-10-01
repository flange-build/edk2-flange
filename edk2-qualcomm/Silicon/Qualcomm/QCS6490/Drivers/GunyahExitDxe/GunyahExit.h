/** @file
  What the switch to EL2 (AArch64/GunyahExit.S) records, for the log.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef GUNYAH_EXIT_H_
#define GUNYAH_EXIT_H_

//
// Offsets into GUNYAH_EXIT_STATE, for the assembly.
//
#define GUNYAH_EXIT_EL1_SCTLR    0x00
#define GUNYAH_EXIT_EL1_TCR      0x08
#define GUNYAH_EXIT_EL1_MAIR     0x10
#define GUNYAH_EXIT_EL1_TTBR0    0x18
#define GUNYAH_EXIT_EL1_VBAR     0x20
#define GUNYAH_EXIT_EL1_SP       0x28
#define GUNYAH_EXIT_CURRENT_EL   0x30
#define GUNYAH_EXIT_RESULT       0x38
#define GUNYAH_EXIT_LEFT_HCR     0x40
#define GUNYAH_EXIT_LEFT_SCTLR   0x48
#define GUNYAH_EXIT_LEFT_TCR     0x50
#define GUNYAH_EXIT_LEFT_VBAR    0x58
#define GUNYAH_EXIT_LEFT_CPTR    0x60
#define GUNYAH_EXIT_LEFT_CNTVOFF 0x68
#define GUNYAH_EXIT_LEFT_DAIF    0x70
#define GUNYAH_EXIT_LEFT_SP      0x78
#define GUNYAH_EXIT_EL2_TCR      0x80
#define GUNYAH_EXIT_ISR          0x88
#define GUNYAH_EXIT_STATE_SIZE   0x90

#ifndef __ASSEMBLER__

typedef struct {
  UINT64    El1Sctlr;     // EL1 registers as UEFI ran with them
  UINT64    El1Tcr;
  UINT64    El1Mair;
  UINT64    El1Ttbr0;
  UINT64    El1Vbar;
  UINT64    El1Sp;
  UINT64    CurrentEl;    // 1 or 2 after the call
  UINT64    Result;       // x0 Gunyah returned
  UINT64    LeftHcr;      // EL2 registers as Gunyah left them
  UINT64    LeftSctlr;
  UINT64    LeftTcr;
  UINT64    LeftVbar;
  UINT64    LeftCptr;
  UINT64    LeftCntvoff;
  UINT64    LeftDaif;     // DAIF, with SPSel in bit 0
  UINT64    LeftSp;
  UINT64    El2Tcr;       // TCR_EL2 this code set
  UINT64    Isr;          // ISR_EL1 at EL2 with the MMU on: pending SError/IRQ/FIQ
} GUNYAH_EXIT_STATE;

/**
  Asks Gunyah to leave with the SMC the stock firmware uses, and when it has,
  turns the MMU back on at EL2 with UEFI's EL1 translation tables, the way
  the stock EnvDxe does. Interrupts stay masked.

  @param[in, out]  State  What was found, for the log.

  @return  x0 of the call: 0 when Gunyah left.
**/
UINT64
EFIAPI
GunyahExitSwitch (
  IN OUT GUNYAH_EXIT_STATE  *State
  );

#endif // __ASSEMBLER__

#endif // GUNYAH_EXIT_H_
