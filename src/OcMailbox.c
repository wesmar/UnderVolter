// OcMailbox.c — Intel OC Mailbox (MSR 0x150) high-level interface:
//               MSR mailbox initialization, interface DWORD construction, and
//               read/write dispatch through the generic CpuMailbox layer.
#include <Uefi.h>

#include "CpuMailboxes.h"
#include "VfCurve.h"

/*******************************************************************************
 * Layout of the CPU overclocking mailbox can be found in academic papers:
 * 
 * DOI:10.1109/SP40000.2020.00057
 * "Figure 1: Layout of the undocumented undervolting MSR with address 0x150"
 *         
 * Command IDs are also often mentioned in BIOS or in officially released Intel
 * Firmware Support Packages, e.g.:
 * 
 * https://github.com/intel/FSP/blob/master/CometLakeFspBinPkg/ \
 * CometLake1/Include/FspmUpd.h
 * 
 * V/F Curve theory-of-operation (for wide audience) can be seen explained
 * in this ScatterBencher video: https://www.youtube.com/watch?v=0TGcKyXBQ6U
 * 
 ******************************************************************************/


/*******************************************************************************
 * OC Mailbox Constants
 ******************************************************************************/

#define OC_MAILBOX_MAX_SPINS                                             999
#define OC_MAILBOX_LATENCY_US                                              9
#define OC_MAILBOX_MAX_RETRIES                                             9

#define OC_MAILBOX_CMD_MASK                                       0x000000ff
#define OC_MAILBOX_BUSY_FLAG_BIT                                  0x80000000
#define OC_MAILBOX_COMPLETION_MASK                                0x000000ff


/*******************************************************************************
 * InitiazeAsMsrOCMailbox
 ******************************************************************************/

// Configure a CpuMailbox for the OC Mailbox MSR (0x150).
// Sets transport type, busy flag, status mask, and retry/timeout parameters.
EFI_STATUS EFIAPI OcMailbox_InitializeAsMSR(CpuMailbox* b)
{
  EFI_STATUS state = EFI_SUCCESS;

  b->status = 0;
  b->b.u64 =  0;
  
  b->cfg.type =       MAILBOX_MSR;  
  b->cfg.addr =       MSR_OC_MAILBOX;
  b->cfg.busyFlag =   OC_MAILBOX_BUSY_FLAG_BIT;
  b->cfg.cmdBits =    OC_MAILBOX_COMPLETION_MASK;
  b->cfg.statusBits = OC_MAILBOX_COMPLETION_MASK;
  b->cfg.maxRetries = OC_MAILBOX_MAX_RETRIES;
  b->cfg.maxSpins =   OC_MAILBOX_MAX_SPINS;
  b->cfg.latency =    OC_MAILBOX_LATENCY_US;

  return state;
}


/*******************************************************************************
 * OcMailbox_Write
 ******************************************************************************/

// Write cmd into the interface field and data into the payload field,
// then delegate to the generic CpuMailbox_ReadWrite transport dispatcher.
EFI_STATUS EFIAPI OcMailbox_ReadWrite( IN CONST UINT32 cmd,
                                       IN CONST UINT32 data,
                                       IN OUT CpuMailbox *b )
{
  b->b.box.ifce = cmd;
  b->b.box.data = data;

  return CpuMailbox_ReadWrite(b);
}

/*******************************************************************************
 * OcMailbox_BuildInterface
 ******************************************************************************/

// Pack the 32-bit OC Mailbox interface word:
//   bits  7:0  = command ID (e.g. 0x10=read VF, 0x11=write VF, 0x16=read IccMax)
//   bits 15:8  = param1 (domain index or VR address)
//   bits 23:16 = param2 (V/F point index, 0 for legacy)
UINT32 OcMailbox_BuildInterface( IN CONST UINT8 cmd,
                                 IN CONST UINT8 param1,
                                 IN CONST UINT8 param2
)
{
  UINT32 iface = cmd;

  iface |= ((UINT32)(param1) << 8);
  iface |= ((UINT32)(param2) << 16);

  return iface;
}
