// Config.h — INI-file parsing, global runtime flags, and policy application entry point.
#pragma once

#include <Uefi.h>

///
/// MINIMAL TRACING
///
/// If UnderVolter.efi is freezing your system during programming and you cannot
/// pinpoint the source, uncomment the ENABLE_MINILOG_TRACING #define, rebuild
/// UnderVolter.efi and run - at the moment of freezing display shall contain
/// last executed modification OPs that can be used to aid debugging
///

//#define ENABLE_MINILOG_TRACING

extern UINT32  gAppDelaySeconds;
extern BOOLEAN gAppQuietMode;
extern BOOLEAN gIniFound;

// Runtime safety / behaviour flags (defined in Config.c, populated from INI).
extern UINT8   gPostProgrammingOcLock;
extern UINT8   gEmergencyExit;
extern UINT8   gEnableSaferAsm;
extern UINT8   gDisableFirmwareWDT;
extern UINT64  gSelfTestMaxRuns;
extern UINT8   gPrintPackageConfig;
extern UINT8   gPrintVFPoints_PostProgram;

VOID LoadAppSettings(VOID);
VOID ReleaseAppSettings(VOID);

// Returns a read-only pointer to the loaded INI buffer, valid between
// LoadAppSettings() and ReleaseAppSettings(). NULL when no INI was found.
CONST CHAR8* GetIniDataPtr(VOID);

// Filesystem containing the successfully loaded configuration.
EFI_HANDLE GetIniDeviceHandle(VOID);
