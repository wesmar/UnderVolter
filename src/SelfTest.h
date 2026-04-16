// SelfTest.h — AVX2 ComboHell stress kernel dispatcher used to validate
//              voltage/frequency stability after programming.
#pragma once

/*******************************************************************************
 * Globals
 ******************************************************************************/

// Number of stress-kernel iterations to execute (0 = skip self-test).
extern UINT64 gSelfTestMaxRuns;

/*******************************************************************************
 * RunPowerManagementSelfTest — run ComboHell AVX2 on every logical CPU and
 *   report any computational errors detected.
 ******************************************************************************/

EFI_STATUS RunPowerManagementSelfTest(VOID);
