// TimeWindows.c — Convert a power-limit window duration (in milliseconds) to
//                 the 5-bit Y mantissa and 2-bit X exponent used by Intel's
//                 tau field in MSR_PACKAGE_POWER_LIMIT and similar registers.
#include <Uefi.h>
#include "VoltTables.h"
#include "Platform.h"

/*******************************************************************************
 * FindTauConsts
 ******************************************************************************/

// Search lookup_taus_5b2b_x1000_shl22[] for the smallest tau >= timeMs and
// return the corresponding (X, Y) pair.  Table is indexed [Y*4 + X].
//
// Each entry is (tau_ms * 1000) << 22; shift by (22 + units) to recover
// tau_ms in the MSR's time unit before comparing against timeMs.
//
// timeMs == MAX_POWAH  → unconstrained: returns (X=3, Y=31) = maximum tau.
// Returns 1 on success, 0 if no matching entry exists (timeMs out of range).
//
// PX: 2-bit exponent field [bits 24:23 of the power-limit DWORD]
// PY: 5-bit mantissa field  [bits 22:17 of the power-limit DWORD]
UINT8 FindTauConsts(
  IN const UINT32 timeMs,         // Input: time in 1/1000s (ms) or MAX_POWAH
  IN const UINT8 units,           // from MSR
  OUT UINT8* PX,                  // [OUT] Calculated X
  OUT UINT8* PY                   // [OUT] Calculated Y
)
{
  const UINT64* ptbl = &lookup_taus_5b2b_x1000_shl22[0];

  if (timeMs != MAX_POWAH)
  {
    for (UINT8 yidx = 0; yidx < 32; yidx++) {
      for (UINT8 xidx = 0; xidx < 4; xidx++) {

        UINT64 entry = *ptbl;
        UINT64 shift = (UINT64)22 + units;

        if ((entry>>shift) >= (UINT64)timeMs)  {
          
          *PX = xidx;
          *PY = yidx;

          return 1;
        }

        ptbl++;
      }
    }
  }
  else {
    *PX = 3;
    *PY = 31;
    return 1;
  }

  return 0;
}