// DelayX86.c — TSC-based spin-delay primitives (nano/microsecond stalls) and
//              TSC frequency calibration via CPUID leaf 0x15 (TSC/crystal ratio)
//              with a CPUID 0x16 fallback for CPUs that report zero crystal freq.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/UefiLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
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
// If the ratio or crystal is missing, measure against the firmware Stall service.
EFI_STATUS EFIAPI InitializeTscVars(VOID)
{
  UINT32 regs[4] = { 0 };
  gTscFreq = gXtalFreq = 0;
  if (gCpuInfo.maxf >= 0x15) AsmCpuidRegisters(0x15, regs);
  gXtalFreq = regs[2];
  if (regs[0] && regs[1] && regs[2]) {
    gTscFreq = ((UINT64)regs[2] * regs[1] + regs[0] / 2) / regs[0];
  } else {
    // Measure against the firmware delay service instead of guessing 2 GHz.
    UINT64 best = MAX_UINT64;
    for (UINTN sample = 0; sample < 3; sample++) {
      UINT64 start = __rdtsc();
      EFI_STATUS status = gBS->Stall(10000);
      UINT64 delta = __rdtsc() - start;
      if (EFI_ERROR(status)) return status;
      if (delta && delta < best) best = delta;
    }
    if (best == MAX_UINT64 || best > MAX_UINT64 / 100) return EFI_DEVICE_ERROR;
    gTscFreq = best * 100;
  }
  return gTscFreq ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/*******************************************************************************
 * StallCpu
 ******************************************************************************/

// Spin-wait for the specified number of TSC ticks using PAUSE to yield to the
// memory subsystem. Unsigned subtraction also handles a TSC wrap-around.
VOID EFIAPI StallCpu(const UINT64 ticks)
{

  UINT64 startTicks = __rdtsc();

  while (__rdtsc() - startTicks < ticks) {
    _mm_pause();
  }
}

/*******************************************************************************
 * NanoStall
 ******************************************************************************/

// Keep the full product until division; long delays must not wrap at 64 bits.
static UINT64 ScaleTicks(UINT64 Value, UINT64 Multiplier, UINT64 Divisor)
{
  if (!Divisor) return 0;
#if defined(_MSC_VER)
  UINT64 High, Remainder;
  UINT64 Low = _umul128(Value, Multiplier, &High);
  if (High >= Divisor) return MAX_UINT64;
  return _udiv128(High, Low, Divisor, &Remainder);
#else
  __uint128_t Result = (__uint128_t)Value * Multiplier / Divisor;
  return Result > MAX_UINT64 ? MAX_UINT64 : (UINT64)Result;
#endif
}

// Convert ns to TSC ticks (ns * gTscFreq / 1e9) and spin-wait.
VOID EFIAPI NanoStall (const UINT64 ns)
{
  UINT64 ticks = ScaleTicks(ns, gTscFreq, 1000000000u);

  StallCpu(ticks);
}

/*******************************************************************************
 * MicroStall
 ******************************************************************************/

// Convert µs to TSC ticks (us * gTscFreq / 1e6) and spin-wait.
VOID EFIAPI MicroStall(const UINT64 us)
{
  UINT64 ticks = ScaleTicks(us, gTscFreq, 1000000u);

  StallCpu(ticks);
}

/*******************************************************************************
 * TicksToNanoSeconds
 ******************************************************************************/

// Convert a raw TSC delta to nanoseconds: Ticks * 1e9 / gTscFreq.
UINT64 EFIAPI TicksToNanoSeconds(UINT64 Ticks)  
{
  return ScaleTicks(Ticks, 1000000000ULL, gTscFreq);
}

/*******************************************************************************
 * ReadTsc
 ******************************************************************************/

UINT64 ReadTsc(VOID)
{
  return __rdtsc();
}
