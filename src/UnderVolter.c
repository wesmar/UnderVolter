#include <PiPei.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Protocol/MpService.h>

#include "Platform.h"
#include "HwAccess.h"
#include "DelayX86.h"
#include "InterruptHook.h"
#include "SelfTest.h"
#include "MiniLog.h"
#include "CpuInfo.h"
#include "CpuData.h"

// Refactored UI & Safety modules
#include "ConsoleUi.h"
#include "SafetyPrompts.h"
#include "UiConsole.h"
#include "PrintStats.h"

#include "Config.h"
#include "NvramSetup.h"
#include "SecureBootEnroll.h"

/*******************************************************************************
 * Globals
 ******************************************************************************/

extern UINT8 gEnableSaferAsm;
extern UINT8 gDisableFirmwareWDT;

extern EFI_BOOT_SERVICES* gBS;
extern EFI_SYSTEM_TABLE*  gST;

extern CONST UINT32 _gUefiDriverRevision = 0;
CHAR8* gEfiCallerBaseName = "UnderVolter";
EFI_STATUS EFIAPI UefiUnload(IN EFI_HANDLE ImageHandle) { return EFI_SUCCESS; }

EFI_MP_SERVICES_PROTOCOL* gMpServices = NULL;
BOOLEAN gCpuDetected = 0;
PLATFORM* gPlatform = NULL;
UINTN gBootCpu = 0;

/*******************************************************************************
 * InitializeUefiEnvironment
 ******************************************************************************/

// One-time UEFI setup: locate EFI_MP_SERVICES_PROTOCOL (BSP index via WhoAmI),
// optionally install the monkey-ISR exception handler, discover MCHBAR/PCIe
// MMIO base, and disable the firmware watchdog timer if requested.
EFI_STATUS InitializeUefiEnvironment(IN EFI_SYSTEM_TABLE* SystemTable)
{
  EFI_STATUS status = EFI_SUCCESS;

  // Get handle to MP Services Protocol
  status = SystemTable->BootServices->LocateProtocol(
    &gEfiMpServiceProtocolGuid, NULL, (VOID**)&gMpServices);

  if (EFI_ERROR(status)) {
    if (!gAppQuietMode) {
      UiPrint(L"[ERROR] Unable to locate firmware MP services"
        "protocol, error code: 0x%x\n", status);
    }
  }

  if (gMpServices) {
    gMpServices->WhoAmI(gMpServices, &gBootCpu);
  }

  InitializeTrace();

  // Hook the BSP with our "SafeAsm" interrupt handler
  if (gEnableSaferAsm) {
    InstallSafeAsmExceptionHandler();
  }

  // Collect the addresses of the buses/devices/etc...
  // so that we do not need to do it every time we need them
  InitializeMMIO();

  // Disable UEFI watchdog timer (if requested)
  if (gDisableFirmwareWDT) {
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
  }

  return status;
}

/*******************************************************************************
 * UefiMain — application entry point
 ******************************************************************************/

// Execution order:
//   1. Load INI settings (Config.c)
//   2. Init GOP console (UiConsole.c)
//   3. CPU detection; warn and prompt if unknown
//   4. Calibrate TSC
//   5. Startup animation + InitializeUefiEnvironment
//   6. Platform discovery (topology, VR, IccMax)
//   7. Emergency-exit countdown (ESC to abort)
//   8. ApplyPolicy: V/F, power limits, turbo ratios
//   9. Optional AVX2 self-test
//  10. Display results table, optional delay loop, cleanup
EFI_STATUS EFIAPI UefiMain(
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE* SystemTable
)
{
  LoadAppSettings();

  if (!gAppQuietMode) {
    UiConsoleInit(SystemTable);
  }

  // Apply NVRAM Setup variable patches from [SetupVar] INI section.
  // No-op when NvramPatchEnabled = 0 (default). If NvramPatchReboot = 1,
  // issues a warm reset here and does not return — patches activate on next POST.
  ApplyNvramSetupPatches(SystemTable);

  // Gather basic CPU info
  gCpuDetected = DetectCpu();

  if (!gCpuDetected) {
    // Throw warning for UNKNOWN CPUs
    if (!gAppQuietMode && !DisplayUnknownCpuWarning()) {
      ReleaseAppSettings();
      return EFI_ABORTED;
    }
  }

  // Set-up TSC timing
  // NOTE: not MP-proofed - multiple packages will use the same calibration
  if (EFI_ERROR(InitializeTscVars())) {
    if (!gAppQuietMode) {
      UiPrint(L"[ERROR] Unable to initialize timing using CPUID leaf 0x15\n");
    }
  }

  if (!gAppQuietMode) {
    RunStartupAnimation();
  }

  // Enroll Secure Boot keys — placed after the animation so [SBE] messages
  // remain visible on screen.  A successful SelfEnroll reboots the system here
  // (before any voltage changes are applied).
  EnrollSecureBootKeys(ImageHandle, SystemTable);

  InitializeUefiEnvironment(SystemTable);
  StartupPlatformInit(SystemTable, &gPlatform);

  if (!gAppQuietMode && CheckForEmergencyExit()) {
    if (gEnableSaferAsm) {
      RemoveAllInterruptOverrides();
    }
    ReleaseAppSettings();
    return EFI_SUCCESS;
  }

  ApplyPolicy(SystemTable, gPlatform);

  if (gSelfTestMaxRuns && !gAppQuietMode) {
    RunPowerManagementSelfTest();
  }

  if (gEnableSaferAsm) {
    RemoveAllInterruptOverrides();
  }

  if (!gAppQuietMode) {
    UiSetAttribute(EFI_WHITE);
    UiAsciiPrint("Finished.\n");
  }

  // Show voltage domain table after programming — reflects actually applied values
  if (gPlatform && !gAppQuietMode) {
    UiClearAnimationArea();
    PrintPlatformSettings(gPlatform);
  }

  if (gAppDelaySeconds > 0) {
    if (!gAppQuietMode) {
      BOOLEAN aborted = FALSE;
      for (UINT32 i = gAppDelaySeconds; i > 0; i--) {
        UiAsciiPrint("\rTime to exit: %u s... (Press ANY KEY to exit) ", i);
        
        for (UINTN j = 0; j < 100; j++) {
          EFI_STATUS Status = SystemTable->BootServices->CheckEvent(SystemTable->ConIn->WaitForKey);
          if (!EFI_ERROR(Status)) {
            EFI_INPUT_KEY Key;
            SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);
            aborted = TRUE;
            break;
          }
          SystemTable->BootServices->Stall(10000); // 10ms
        }
        if (aborted) break;
      }
      UiAsciiPrint("\r                                                        \r");
    } else {
      SystemTable->BootServices->Stall(gAppDelaySeconds * 1000000);
    }
  }

  ReleaseAppSettings();
  return EFI_SUCCESS;
}
