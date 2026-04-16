// DelayX86.h — TSC-based spin-delay primitives (nano/microsecond stalls) and
//              TSC-frequency calibration via CPUID leaf 0x15/0x16.
#pragma once

/*******************************************************************************
 * InitializeTscVars() gets and stores the characteristics of the TSC counter
 * this is necessary for accurate timing
 ******************************************************************************/

EFI_STATUS EFIAPI InitializeTscVars(VOID);

/*******************************************************************************
 * nsDelay
 * Stalls the CPU for specific number of ns (nanoseconds)
 ******************************************************************************/

VOID EFIAPI NanoStall(const UINT64 ns);

/*******************************************************************************
 * usDelay
 * Stalls the CPU for specific number of ns (microseconds)
 ******************************************************************************/

VOID EFIAPI MicroStall(const UINT64 us);

/*******************************************************************************
 * TicksToNanoSeconds
 ******************************************************************************/

UINT64 EFIAPI TicksToNanoSeconds(UINT64 Ticks);

/*******************************************************************************
 * ReadTsc
 ******************************************************************************/

UINT64 ReadTsc(VOID);