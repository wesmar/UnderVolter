// CpuMailboxes.c — Generic CPU mailbox engine: busy-wait polling, MSR write-then-
//                  readback with retry, and the CpuMailbox_ReadWrite dispatcher.
//                  Supports MSR transport; MMIO path exists but is not yet wired.
#include <Uefi.h>
#include "HwAccess.h"
#include "DelayX86.h"
#include "CpuMailboxes.h"

/*******************************************************************************
 * Layout of the CPU Overclocking mailbox can be found in academic papers:
 *
 * DOI: 10.1109/SP40000.2020.00057 *
 * "Figure 1: Layout of the undocumented undervolting MSR with address 0x150"
 *
 * Command IDs are also often mentioned in BIOS configuration help, or in
 * officially released Intel Firmware Support Packages, e.g.:
 *
 * https://github.com/intel/FSP/blob/master/CometLakeFspBinPkg/ \
 * CometLake1/Include/FspmUpd.h  (merge this line with the hyper link)
 *
 * V/F Curve theory-of-operation (for wide audience) can be seen explained
 * in this ScatterBencher video: https://www.youtube.com/watch?v=0TGcKyXBQ6U
 ******************************************************************************/

/*******************************************************************************
 * CpuMailbox_MMIOBusyWait
 ******************************************************************************/

EFI_STATUS EFIAPI CpuMailbox_MMIOBusyWait(CpuMailbox* b)
{
  const UINT32 mmioAddr = b->cfg.addr;
  const UINT32 busyFlag = b->cfg.busyFlag;
  const UINT32 maxSpins = b->cfg.maxSpins;

  MailboxBody* mb = &b->b;

  UINT8 busy = 1;
  UINTN nspins = 0;

  do {
    mb->box.ifce = pm_mmio_read32(mmioAddr);

    busy = (mb->box.ifce & busyFlag) ? 1 : 0;

    if (busy) {
      nspins++;
      MicroStall(1);
    }
  } while ((busy) && (nspins < maxSpins));

  //
  // Check if the operation timed out

  if ((busy) && (nspins >= maxSpins)) {
    return EFI_ABORTED;
  }

  return EFI_SUCCESS;
}


/*******************************************************************************
 * CpuMailbox_BusyWait_MSR
 ******************************************************************************/

EFI_STATUS EFIAPI CpuMailbox_MsrBusyWait(CpuMailbox *b)
{
  const UINT32 msrIdx =   b->cfg.addr;
  const UINT32 busyFlag = b->cfg.busyFlag;
  const UINT32 maxSpins = b->cfg.maxSpins;

  UINT8 busy = 1;
  UINTN nspins = 0;

  do {
    b->b.u64 = pm_rdmsr64(msrIdx);
    
    busy = (b->b.box.ifce & busyFlag) ? 1 : 0;

    if (busy) {
      nspins++;
      MicroStall(1);
    }
  } while ((busy) && (nspins < maxSpins));

  //
  // Check if the operation timed out

  if ((busy) && (nspins >= maxSpins)) {
    return EFI_ABORTED;
  }

  return EFI_SUCCESS;
}

/*******************************************************************************
 * CpuMailbox_MsrReadWrite
 ******************************************************************************/

EFI_STATUS EFIAPI CpuMailbox_MsrReadWrite( CpuMailbox *b )
{
  const UINT64 request = b->b.u64;
  EFI_STATUS state = EFI_DEVICE_ERROR;
  for (UINT32 attempt = 0; attempt < b->cfg.maxRetries; attempt++) {
    // BusyWait overwrites the body. Preserve the original command across retries.
    if (EFI_ERROR(CpuMailbox_MsrBusyWait(b))) return EFI_TIMEOUT;
    b->b.u64 = request;
    b->b.box.ifce |= b->cfg.busyFlag;
    if (pm_wrmsr64(b->cfg.addr, b->b.u64)) return EFI_DEVICE_ERROR;
    if (EFI_ERROR(CpuMailbox_MsrBusyWait(b))) return EFI_TIMEOUT;
    MailboxBody result = b->b;
    MicroStall(b->cfg.latency);
    b->b.u64 = pm_rdmsr64(b->cfg.addr);
    if (b->b.u64 != result.u64) continue;
    b->status = b->b.box.ifce & b->cfg.statusBits;
    return b->status ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  }
  return state;
}

/*******************************************************************************
 * CpuMailbox_ReadWrite
 ******************************************************************************/

EFI_STATUS EFIAPI CpuMailbox_ReadWrite(CpuMailbox* b)
{
  switch (b->cfg.type)
  {
    case MAILBOX_MSR:
    {
      return CpuMailbox_MsrReadWrite(b);
    }
    break;

    default:
    {
      return EFI_INVALID_PARAMETER;
    }
    break;
  }  
}
