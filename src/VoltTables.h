// VoltTables.h — Pre-computed lookup tables mapping millivolt values to/from the
//                OC Mailbox fixed-point voltage encoding (S11 offset, U12 override,
//                and 5-bit/2-bit tau window constants).
#pragma once

// OffsetVolts_S11: 256-entry table, signed 11-bit fixed-point offset encoding,
//   covers ±250 mV in ~1 mV steps.  Each UINT64 packs four 16-bit {fx,mv} pairs.
extern const UINT64 OffsetVolts_S11[256];

// OverrdVolts_U12: 640-entry table, unsigned 12-bit absolute voltage encoding,
//   covers 250 mV–1500 mV.  Same packing as OffsetVolts_S11.
extern const UINT64 OverrdVolts_U12[640];

// lookup_taus_5b2b_x1000_shl22: 128-entry (32 Y × 4 X) table for the 5-bit Y
//   and 2-bit X tau window exponent encoding used in MSR_PACKAGE_POWER_LIMIT.
//   Entry = tau_ms * 1000, pre-shifted left by 22 so FindTauConsts can compare
//   directly after right-shifting by (22 + timeUnits).
extern const UINT64 lookup_taus_5b2b_x1000_shl22[128];