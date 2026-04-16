// DelayX86.c — TSC-based spin-delay primitives (nano/microsecond stalls) and
//              TSC frequency calibration via CPUID leaf 0x15 (TSC/crystal ratio)
//              with a CPUID 0x16 fallback for CPUs that report zero crystal freq.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/UefiLib.h>
#include <Library/TimerLib.h>
#include "CpuInfo.h"
#include "CpuData.h"
#include "HwIntrinsicsX64.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__clang__)
#include <immintrin.h>
#endif

#if defined(__GNUC__) && !defined(__clang__)
#include <x86intrin.h>
#else

#pragma intrinsic(__rdtsc)                // At this point, code will look so
#pragma intrinsic(_mm_pause)              // fugly that writing it in pure SMM
                                          // ASM would count as an improvement
#endif


/*******************************************************************************
 * Globals that must be initialized
 ******************************************************************************/

UINT64 gTscFreq = 0;
UINT64 gXtalFreq = 0;

/*******************************************************************************
 * InitializeTscVars
 ******************************************************************************/

// Calibrate gTscFreq (TSC ticks/second) using CPUID leaf 0x15:
//   leaf 0x15: eax=denominator, ebx=numerator, ecx=crystal_Hz (may be 0)
//   TSC freq = crystal * ebx / eax
// If crystal is not reported (ecx==0), derive it from CPUID 0x16 base MHz.
// On QEMU or hypervisors where 0x15 returns zero, falls back to a hard-coded
// 124.783 MHz default (prevents divide-by-zero; stalls will be inaccurate).
EFI_STATUS EFIAPI InitializeTscVars(VOID)
{
  UINT64 tmp;
  UINT32 regs[4] = { 0 };

  if(gCpuInfo.maxf >= 0x15)
    AsmCpuidRegisters(0x15, regs);

  gXtalFreq = regs[2];  // ecx = nominal core crystal clock Hz

  if ((regs[0] == 0) || (regs[1] == 0)) {

    //
    // Totally bogus values
    // TODO: calibrate using some known clock source

    regs[0] = regs[1] = 1;
    gTscFreq = 124783;
    
    return EFI_SUCCESS;
  }

  if(gXtalFreq == 0)
  {
    UINT32 regs2[4] = { 0 };

    if (gCpuInfo.maxf >= 0x15)
      AsmCpuidRegisters(0x16, regs2);

    // Derive crystal from base CPU MHz (leaf 0x16 eax) * ratio
    gXtalFreq = (UINT64)regs2[0] * 1000000 * (UINT64)regs[0] / (UINT64)regs[1];

    if (gXtalFreq == 0) {
      gXtalFreq = 23958333;  // 24 MHz nominal; no reliable way to probe SKU here
    }
  }

  // TSC freq = crystal * ebx / eax  (rounded)
  tmp = (UINT64)gXtalFreq * (UINT64)regs[1];

  if (regs[0] > 1) {
    tmp += (UINT64)regs[0] >> 1;  // round half-up before integer divide
    tmp /= (UINT64)regs[0];
  }

  gTscFreq = (UINT32)tmp;

  return EFI_SUCCESS;
}

/*******************************************************************************
 * StallCpu
 ******************************************************************************/

// Spin-wait for the specified number of TSC ticks using PAUSE to yield to the
// memory subsystem.  TSC wrap-around is not guarded (counter resets after ~292
// years at 1 GHz; irrelevant in a UEFI pre-boot context).
VOID EFIAPI StallCpu(const UINT64 ticks)
{

  UINT64 endTicks = __rdtsc() + ticks;

  while (__rdtsc() <= endTicks) {
    _mm_pause();
  }
}

/*******************************************************************************
 * NanoStall
 ******************************************************************************/

// Convert ns to TSC ticks (ns * gTscFreq / 1e9) and spin-wait.
VOID EFIAPI NanoStall (const UINT64 ns)
{
  UINT64 ticks = ns * gTscFreq / 1000000000u;

  StallCpu(ticks);
}

/*******************************************************************************
 * MicroStall
 ******************************************************************************/

// Convert µs to TSC ticks (us * gTscFreq / 1e6) and spin-wait.
VOID EFIAPI MicroStall(const UINT64 us)
{
  UINT64 ticks = us * gTscFreq / 1000000u;

  StallCpu(ticks);
}

/*******************************************************************************
 * TicksToNanoSeconds
 ******************************************************************************/

// Convert a raw TSC delta to nanoseconds: Ticks * 1e9 / gTscFreq.
UINT64 EFIAPI TicksToNanoSeconds(UINT64 Ticks)  
{
  return (UINT64)(1000000000u * Ticks) / gTscFreq;
}

/*******************************************************************************
 * ReadTsc
 ******************************************************************************/

UINT64 ReadTsc(VOID)
{
  return __rdtsc();
}
