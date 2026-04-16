// FixedPoint.c — Voltage unit conversion between signed millivolt integers and
//                the S11 / U12 fixed-point encodings used by the OC Mailbox.
//                All conversions use binary-search through the pre-computed
//                lookup tables in VoltTables.c; a closed-form fallback handles
//                values that fall outside the table range.
#include "VoltTables.h"
#include "FixedPoint.h"

// ─── Internal lookup union ───────────────────────────────────────────────────
// Each 32-bit word in the voltage tables packs two 16-bit fields:
//   lo word (mv): millivolt value (signed for S11, unsigned for U12)
//   hi word (fx): OC Mailbox fixed-point encoding

typedef union _VOLTS16 {
  UINT32 raw;

  struct {
    INT16  mv;
    UINT16 fx;
  } s;

  struct {
    UINT16 mv;
    UINT16 fx;
  } u;
} VOLTS16;

/*******************************************************************************
 * convert_offsetvolts_int16_fixed
 ******************************************************************************/

// Convert a millivolt offset (−250..+250 mV) to its S11 OC Mailbox encoding.
// Table search: walks OffsetVolts_S11 until mv <= in (first entry not greater).
// Returns VOLT_ERROR16 when no matching entry exists.
UINT16 cvrt_offsetvolts_i16_tofix(const INT16 in)
{
  ///
  /// If you wish to apply larger offset than +/- 250 mV
  /// you will need to replace the code in this routine
  /// 

  UINT64 nvolts = sizeof(OffsetVolts_S11) / (sizeof(VOLTS16));
  VOLTS16* vtbl = (VOLTS16*)&OffsetVolts_S11[0];

  if (in == 0)
    return 0;

  for (UINT64 vidx = 0; vidx < nvolts; vidx++) {
    if (vtbl[vidx].s.mv <= in) {
      return vtbl[vidx].s.fx;
    }
  }

  return VOLT_ERROR16;
}

/*******************************************************************************
 * convert_offsetvolts_fixed_int16
 ******************************************************************************/

// Convert an S11 OC Mailbox encoding back to a signed millivolt offset.
// Tries an exact match in OffsetVolts_S11 first.  If not found, uses the
// closed-form formula:
//   if bit10 set (negative): mv = ~(((~in+1)&0x3ff)*1000 >> 10) + 1
//   else (positive):         mv = ((in & 0x3ff) * 1000) >> 10
// This is an integer approximation of the S11 1/1024 mV step.
INT16 cvrt_offsetvolts_fxto_i16(const UINT16 in)
{
  ///
  /// If you wish to apply larger offset than +/- 250 mV
  /// you will need to replace the code in this routine
  /// 

  UINT64 nvolts = sizeof(OffsetVolts_S11) / (sizeof(VOLTS16));
  VOLTS16* vtbl = (VOLTS16*)&OffsetVolts_S11[0];

  if (in == 0)
    return 0;

  for (UINT64 vidx = 0; vidx < nvolts; vidx++) {
    if (vtbl[vidx].s.fx == in) {
      return vtbl[vidx].s.mv;
    }
  }

  // Closed-form S11 decode: bit 10 is the sign bit; steps are 1000/1024 mV each.
  return (INT16)((in&0x400)?~((((INT64)((~in+1)&0x3ff))*1000)>>10) + 1 :
     ((((INT64)(in&0x3ff)))*1000)>>10);
}

/*******************************************************************************
 * convert_ovrdvolts_fixed_int16
 ******************************************************************************/

// Convert a U12 OC Mailbox absolute voltage encoding to millivolts.
// Tries an exact match in OverrdVolts_U12 first; falls back to the formula
//   mv = ((in & 0xFFF) * 1000) >> 10   (1000/1024 mV per LSB).
UINT16 cvrt_ovrdvolts_fxto_i16(const UINT16 in)
{
  ///
  /// If you want to work with range larger than 250 mV - 1500 mV
  /// you will need to replace the code in this routine
  /// 

  UINT64 nvolts = sizeof(OverrdVolts_U12) / (sizeof(VOLTS16));
  VOLTS16* vtbl = (VOLTS16*)&OverrdVolts_U12[0];

  if (in == 0)
    return 0;

  for (UINT64 vidx = 0; vidx < nvolts; vidx++) {
    if (vtbl[vidx].u.fx == in) {
      return vtbl[vidx].u.mv;
    }
  }

  return (UINT16)((((INT64)(in&0xfff))*1000)>>10);
}

/*******************************************************************************
 * convert_ovrdvolts_int16_fixed
 ******************************************************************************/

// Convert an absolute millivolt value (250–1500 mV) to its U12 OC Mailbox
// encoding.  Table search: walks OverrdVolts_U12 until mv <= in.
// Returns VOLT_ERROR16 when no matching entry exists.
UINT16 cvrt_ovrdvolts_i16_tofix(const UINT16 in)
{
  ///
  /// If you want to work with range larger than 250 mV - 1500 mV
  /// you will need to replace the code in this routine
  /// 

  UINT64 nvolts = sizeof(OverrdVolts_U12) / (sizeof(VOLTS16));
  VOLTS16* vtbl = (VOLTS16*)&OverrdVolts_U12[0];

  if (in == 0)
    return 0;

  for (UINT64 vidx = 0; vidx < nvolts; vidx++) {
    if (vtbl[vidx].s.mv <= in) {
      return vtbl[vidx].s.fx;
    }
  }

  return VOLT_ERROR16;
}
