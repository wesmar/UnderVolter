// CpuStressorAvx2.h — Interface to the AVX2 ComboHell stress kernel:
//                     mixed integer XOR and floating-point VRCPPS/VDPPS operations
//                     exercising all YMM registers and the FP pipeline simultaneously.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

  // Run the AVX2 stress loop.  in: pointer to cbhell_ymm_input (32-byte aligned),
  // out: pointer to 64-UINT64 scratch buffer (32-byte aligned).
  // Returns 0 on clean stop, 0xBADDC0DE on computational error detected.
  UINT64 RunAvx2StressKernel( void *in, void *out );

  // Pointer to a UINT32 stop flag; kernel polls this between iterations.
  extern void* ComboHell_StopRequestPtr;

  // Pointer to a UINT64 error counter; incremented atomically on each mismatch.
  extern void* ComboHell_ErrorCounterPtr;

  // Maximum number of outer iterations (0 = run until stop is requested).
  extern UINT64 ComboHell_MaxRuns;

  // Non-zero: set the stop flag and exit immediately on first detected error.
  extern UINT64 ComboHell_TerminateOnError;

#ifdef __cplusplus
}
#endif