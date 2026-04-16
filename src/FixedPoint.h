// FixedPoint.h — Voltage unit conversions between signed/unsigned millivolt integers
//                and the OC Mailbox S11/U12 fixed-point wire formats.
#pragma once

// Sentinel returned when a millivolt value has no matching table entry.
#define VOLT_ERROR16                                                     0x7fff

/*******************************************************************************
 * convert_offsetvolts_int16_fixed
 ******************************************************************************/

UINT16 cvrt_offsetvolts_i16_tofix(const INT16 in);

/*******************************************************************************
 * convert_offsetvolts_fixed_int16
 ******************************************************************************/

INT16 cvrt_offsetvolts_fxto_i16(const UINT16 in);

/*******************************************************************************
 * convert_ovrdvolts_fixed_int16
 ******************************************************************************/

UINT16 cvrt_ovrdvolts_fxto_i16

(const UINT16 in);

/*******************************************************************************
 * convert_ovrdvolts_int16_fixed
 ******************************************************************************/

UINT16 cvrt_ovrdvolts_i16_tofix(const UINT16 in);
