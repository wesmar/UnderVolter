// TimeWindows.h — Tau window encoding helper for MSR_PACKAGE_POWER_LIMIT:
//                 converts a time-window in milliseconds to the 5-bit Y and 2-bit X
//                 exponent fields used in Intel power-limit MSRs.
#pragma once

/*******************************************************************************
 * FindTauConsts — look up (X, Y) such that tau = Y * 2^X * time_unit >= timeMs.
 *   Returns 1 on success, 0 if no valid encoding exists.
 *   When timeMs == MAX_POWAH, returns the maximum representable tau (X=3, Y=31).
 ******************************************************************************/

UINT8 FindTauConsts(
  IN const UINT32 timeMs,         // Desired window in ms, or MAX_POWAH for maximum
  IN const UINT8 units,           // Time unit exponent read from MSR_PACKAGE_POWER_SKU_UNIT
  OUT UINT8* PX,                  // [OUT] 2-bit X exponent
  OUT UINT8* PY                   // [OUT] 5-bit Y mantissa
);
