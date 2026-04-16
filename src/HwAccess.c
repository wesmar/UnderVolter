// HwAccess.c — Fault-safe hardware I/O wrappers: MSR read/write and MMIO
//              read/write/OR, all routed through the monkey-ISR mechanism so
//              that a #GP or #UD caused by an unsupported register triggers
//              HandleProbingFault rather than a triple-fault.  Also provides
//              GS-base accessors (per-CPU local storage), MCHBAR discovery,
//              and unified MSR/MMIO dispatchers (pm_xio_read64/write64).
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__clang__)
#include <immintrin.h>
#endif

#include "HwIntrinsicsX64.h"
#include "HwAccess.h"
#include "MiniLog.h"

/*******************************************************************************
 * Compiler Overrides
 ******************************************************************************/

#if defined(_MSC_VER)
#pragma warning( disable : 4090 )
#endif

#if defined(__GNUC__) && !defined(__clang__)
#include <x86intrin.h>
#else

#pragma intrinsic(__rdtsc)                // At this point, code will look so
#pragma intrinsic(_mm_pause)              // fugly that writing it in pure SMM
                                          // ASM would count as an improvement
#endif


/*******************************************************************************
 * Globals
 ******************************************************************************/

UINT64 gFatalErrorAsserted = 0;
UINT32 gPCIeBaseAddr = 0;
UINT32 gMCHBAR = 0;

/*******************************************************************************
 *
 ******************************************************************************/

typedef struct _MINISTAT {
  
  CHAR8 errtxt[128];

  UINT64 param1;
  UINT64 param2;
  UINT64 param3;

} MINISTAT;

/*******************************************************************************
 * HandleProbingFault
 ******************************************************************************/

// Called when a Safe{Read,Write}Msr64 or SafeMmio* call returns a non-zero
// error code (fault was caught by the monkey-ISR).  Logs the operation name
// and address, then freezes this CPU with interrupts disabled.  The atomic
// increment on gFatalErrorAsserted ensures the message is printed only once
// even when multiple APs fault simultaneously.
VOID EFIAPI HandleProbingFault(MINISTAT *pst)
{

  //
  // Ensure display happens only once
  
  UINT64 cnt = AtomicIncrementU64(&gFatalErrorAsserted);

  if (cnt == 1)
  {
    /////////////////////////////////////////////////////////////
    // TODO: Fix for MP needed - this will not work on non-BSP //
    /////////////////////////////////////////////////////////////
    
    MiniTraceEx("UnderVolter has encountered a fatal error during operation.\n");    
  }

  //
  // Freeze this CPU
  
  DisableInterruptsOnThisCpu();

  for (;;) {
    _mm_pause();
  }
}

/*******************************************************************************
 * GetCpuGSBase
 ******************************************************************************/

// Read MSR 0xC0000101 (IA32_GS_BASE) via SafeReadMsr64 and return its value
// as a pointer to the calling CPU's CPUCORE block.
VOID* EFIAPI GetCpuGSBase()
{
  UINT32 err;
  return (VOID *)SafeReadMsr64(0xc0000101, &err);
}


/*******************************************************************************
 * SetCpuGSBase
 ******************************************************************************/

// Write addr into MSR 0xC0000101 (IA32_GS_BASE) for the calling CPU.
VOID EFIAPI SetCpuGSBase(const void* addr)
{
  SafeWriteMsr64(0xc0000101, (UINT64)addr);
}


/*******************************************************************************
 * InitializeMMIO
 ******************************************************************************/

// Discover the PCIe ECAM base (via GetPciExpressBaseAddress) and locate MCHBAR
// at PCIe cfg offset 0x48 (Host Bridge D0:F0).  MCHBAR bit0 is the enable
// flag; the actual base address is bits 31:1.  Both globals are zeroed on
// failure so callers can guard with if(gMCHBAR).
VOID EFIAPI InitializeMMIO(VOID)
{

  //
  // We need base of the PCIe address space

  gPCIeBaseAddr = GetPciExpressBaseAddress();

  //
  // Just in case
  // 
  if (!(gPCIeBaseAddr & 0x1))
  {
    gPCIeBaseAddr = 0;
    gMCHBAR = 0;
  }    

  //
  // Locate MCHBAR

  gMCHBAR = pm_mmio_read32((gPCIeBaseAddr & 0xFC000000) + 0x48) & 0xfffffffe;
}

// ─── MSR and MMIO wrappers: each calls the Safe* ASM stub, logs via MiniTrace,
//     and calls HandleProbingFault on any non-zero error code. ─────────────────

// Read a 64-bit MSR.  Logs MINILOG_OPID_RDMSR64; calls HandleProbingFault on #GP.
UINT64 EFIAPI pm_rdmsr64(const UINT32 msr_idx)
{ 
  UINT32 err = 0;
  UINT64 val = SafeReadMsr64(msr_idx, &err);

  MiniTrace(MINILOG_OPID_RDMSR64, 1, (UINT32)msr_idx, (err)?0xBAAD : val);

  if (err) {

    MINISTAT bug = { 0 };

    AsciiStrCpyS(&bug.errtxt[0], 128, "rdmsr64");
    
    bug.param1 = 0;
    bug.param2 = (UINT64) msr_idx;
    bug.param3 = val;

    HandleProbingFault(&bug); 
  }

  return val;
}

// Write a 64-bit MSR.  Logs MINILOG_OPID_WRMSR64; calls HandleProbingFault on #GP.
UINT32 EFIAPI pm_wrmsr64(const UINT32 msr_idx, const UINT64 value)
{
  MiniTrace(MINILOG_OPID_WRMSR64, 1, (UINT32)msr_idx, value);

  UINT32 err = SafeWriteMsr64(msr_idx, value);

  if (err) {

    MINISTAT bug = { 0 };

    AsciiStrCpyS(&bug.errtxt[0], 128, "wrmsr64");

    bug.param1 = 0;
    bug.param2 = (UINT64)msr_idx;
    bug.param3 = value;

    HandleProbingFault(&bug);
  }

  return err;
}

// Read a 32-bit MMIO register at absolute address addr.
UINT32 EFIAPI pm_mmio_read32(const UINT32 addr)
{
  UINT32 err = 0;
  UINT32 val = SafeMmioRead32(addr, &err);

  MiniTrace(MINILOG_OPID_MMIO_READ32, 0, (UINT64)((err) ? 0xBAAD : (UINT64)val) | (UINT64)addr<<32, 0);

  if (err) {

    MINISTAT bug = { 0 };

    AsciiStrCpyS(&bug.errtxt[0], 128, "mmio_read32");

    bug.param1 = 0;
    bug.param2 = (UINT64)addr;
    bug.param3 = 0;

    HandleProbingFault(&bug);
  }

  return val;
}

// Read-modify-write: OR value into the 32-bit MMIO register at addr.
UINT32 EFIAPI pm_mmio_or32(const UINT32 addr, const UINT32 value)
{
  MiniTrace(MINILOG_OPID_MMIO_OR32, 0, (UINT64)value | (UINT64)addr<<32, 1);

  UINT32 err = SafeMmioOr32(addr, value);

  if (err) {

    MINISTAT bug = { 0 };

    AsciiStrCpyS(&bug.errtxt[0], 128, "mmio_or32");

    bug.param1 = 0;
    bug.param2 = (UINT64)addr;
    bug.param3 = (UINT64)value;

    HandleProbingFault(&bug);
  }

  return value;
}

// Write a 32-bit value to MMIO register at addr.
UINT32 EFIAPI pm_mmio_write32(const UINT32 addr, const UINT32 value)
{
  MiniTrace(MINILOG_OPID_MMIO_WRITE32, 0, (UINT64)value | (UINT64)addr << 32, 1);

  UINT32 err = SafeMmioWrite32(addr, value);

  if (err) {

    MINISTAT bug = { 0 };

    AsciiStrCpyS(&bug.errtxt[0], 128, "mmio_write32");

    bug.param1 = 0;
    bug.param2 = (UINT64)addr;
    bug.param3 = (UINT64)value;

    HandleProbingFault(&bug);
  }

  return value;
}

/*******************************************************************************
 * pm_xio_read64
 ******************************************************************************/

// Unified 64-bit read: IO_MSR → pm_rdmsr64(addr); IO_MMIO → two 32-bit MMIO
// reads from gMCHBAR+addr and gMCHBAR+addr+4.
// Returns 0xBADC0DEBADC0DE if MMIO is selected but gMCHBAR was not discovered.
UINT64 EFIAPI pm_xio_read64(const UINT8 tgtype, const UINT32 addr)
{
  if (tgtype == IO_MSR) {
    return pm_rdmsr64(addr);
  }
  else {
    if (gMCHBAR) {

      QWORD msr = { 0 };

      msr.u32.lo = pm_mmio_read32(gMCHBAR + addr);
      msr.u32.hi = pm_mmio_read32(gMCHBAR + addr + 4);

      return msr.u64;
    }
  }

  return 0xbadc0debadc0de;
}

/*******************************************************************************
 * pm_xio_write64
 ******************************************************************************/

// Unified 64-bit write: IO_MSR → pm_wrmsr64; IO_MMIO → two 32-bit MMIO writes
// to gMCHBAR+addr (lo) and gMCHBAR+addr+4 (hi).
UINT32 EFIAPI pm_xio_write64(const UINT8 tgtype, const UINT32 addr, const UINT64 val)
{
  if (tgtype == IO_MSR) {
    return pm_wrmsr64(addr, val);
  }
  else {
    if (gMCHBAR) {

      QWORD msr;

      msr.u64 = val;

      pm_mmio_write32(gMCHBAR + addr, msr.u32.lo);
      return pm_mmio_write32(gMCHBAR + addr + 4, msr.u32.hi);
    }
  }

  return 0xbadc0de;
}
