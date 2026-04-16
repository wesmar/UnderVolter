// OcMailbox.h — High-level wrapper for Intel OC Mailbox (MSR 0x150):
//               initialization, command encoding, and read/write dispatch.
#pragma once

#include "CpuMailboxes.h"

/*******************************************************************************
 * InitiazeAsMsrOCMailbox
 ******************************************************************************/

EFI_STATUS EFIAPI OcMailbox_InitializeAsMSR(CpuMailbox* b);

/*******************************************************************************
 * OcMailbox_Write
 ******************************************************************************/

EFI_STATUS EFIAPI OcMailbox_ReadWrite(
  IN CONST UINT32 cmd,
  IN CONST UINT32 data,
  IN OUT CpuMailbox* b);

/*******************************************************************************
 * OcMailbox_BuildInterface
 ******************************************************************************/

UINT32 OcMailbox_BuildInterface(IN CONST UINT8 cmd,
  IN CONST UINT8 param1,
  IN CONST UINT8 param2
);

