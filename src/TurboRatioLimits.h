// TurboRatioLimits.h — Read/write wrappers for MSR_TURBO_RATIO_LIMIT (0x1AD)
//                      and its E-Core counterpart (0x650); all-core ratio program.
#pragma once

/*******************************************************************************
 * GetTurboRatioLimits
 ******************************************************************************/

UINT64 GetTurboRatioLimits(VOID);
UINT64 GetTurboRatioLimits_ECORE(VOID);

/*******************************************************************************
 * SetTurboRatioLimits
 ******************************************************************************/

EFI_STATUS SetTurboRatioLimits(const UINT64 val);
EFI_STATUS SetEfficientCoreTurboRatioLimits(const UINT64 val);

/*******************************************************************************
 * ProgramMaxTurboRatios
 ******************************************************************************/

EFI_STATUS ProgramMaxTurboRatios(const UINT8 maxRatio);
EFI_STATUS ProgramEfficientCoreMaxTurboRatios(const UINT8 maxRatio);