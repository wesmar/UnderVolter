#include <PiPei.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>     // gBS, gImageHandle
#include <Library/BaseMemoryLib.h>                // SetMem/CopyMem standard
#include <Protocol/MpService.h>
#include <Library/MemoryAllocationLib.h>

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

// gEnableSaferAsm / gDisableFirmwareWDT are defined in Config.c and declared
// in Config.h — included via Config.h above.  No local externs needed.

// gBS / gST come from UefiBootServicesTableLib (included above) — no need to
// re-declare extern in this translation unit.

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

  if (EFI_ERROR(status) || !gMpServices) return EFI_UNSUPPORTED;
  status = gMpServices->WhoAmI(gMpServices, &gBootCpu);
  if (EFI_ERROR(status)) return status;

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
//   3. NVRAM [SetupVar] patches (may reboot — CPU-agnostic, runs first)
//   4. SelfEnroll Secure Boot keys (may reboot — CPU-agnostic, runs second)
//   5. CPU detection; warn and prompt if unknown
//   6. Calibrate TSC
//   7. Startup animation + InitializeUefiEnvironment
//   8. Platform discovery (topology, VR, IccMax)
//   9. Emergency-exit countdown (ESC to abort)
//  10. ApplyPolicy: V/F, power limits, turbo ratios
//  11. Optional AVX2 self-test
//  12. Display results table, optional delay loop, cleanup
//
// Reboot-triggering stages (NVRAM patch, SelfEnroll) are placed before CPU
// detection / TSC calibration / animation so that on the bootstrap reboots
// no CPU/timing work is wasted before the warm reset.  Both stages are
// idempotent — second pass through them is a fast no-op when state already
// matches the desired post-conditions.
EFI_STATUS EFIAPI UefiMain(
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE* SystemTable
)
{
  EFI_STATUS Status = EFI_SUCCESS;
  LoadAppSettings();
  if (gDisableFirmwareWDT) SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

  if (!gAppQuietMode) {
    UiConsoleInit(SystemTable);
  }

  // Apply NVRAM Setup variable patches from [SetupVar] INI section.
  // No-op when NvramPatchEnabled = 0 (default). If NvramPatchReboot = 1
  // and a write was needed, issues a warm reset here and does not return —
  // patches activate on next POST.
  ApplyNvramSetupPatches(SystemTable);

  // Enroll Secure Boot keys from [SecureBoot].  Both .auth-file enrollment
  // and SelfEnroll are CPU-agnostic; placing them here means that on a
  // bootstrap reboot no downstream work (CPU detection, TSC calibration,
  // startup animation) was wasted before the warm reset.
  EnrollSecureBootKeys(ImageHandle, SystemTable);

  // Gather basic CPU info
  gCpuDetected = DetectCpu();

  if (!gCpuDetected) {
    // Throw warning for UNKNOWN CPUs
    if (gAppQuietMode || !DisplayUnknownCpuWarning()) {
      ReleaseAppSettings();
      return EFI_ABORTED;
    }
  }

  // Lunar Lake currently has no supported voltage programming path.
  if (gCpuInfo.family == 6 && gCpuInfo.model == 189) {
    if (!gAppQuietMode) UiPrint(L"Lunar Lake voltage programming is not supported.\n");
    Status = EFI_UNSUPPORTED;
    goto Cleanup;
  }
  if (!gIniFound) {
    if (!gAppQuietMode) UiPrint(L"No usable UnderVolter.ini found; CPU settings were not changed.\n");
    Status = EFI_NOT_FOUND;
    goto Cleanup;
  }

  // Set-up TSC timing
  // NOTE: not MP-proofed - multiple packages will use the same calibration
  Status = InitializeTscVars();
  if (EFI_ERROR(Status)) {
    if (!gAppQuietMode) {
      UiPrint(L"[ERROR] Unable to initialize timing: %r\n", Status);
    }
    goto Cleanup;
  }

  if (!gAppQuietMode) {
    RunStartupAnimation();
  }

  Status = InitializeUefiEnvironment(SystemTable);
  if (EFI_ERROR(Status)) goto Cleanup;
  ProbeBclk();
  Status = StartupPlatformInit(SystemTable, &gPlatform);
  if (EFI_ERROR(Status)) goto Cleanup;

  if (!gAppQuietMode && CheckForEmergencyExit()) {
    goto Cleanup;
  }

  Status = ApplyPolicy(SystemTable, gPlatform);
  if (EFI_ERROR(Status)) goto Cleanup;

  if (gSelfTestMaxRuns && !gAppQuietMode) {
    Status = RunPowerManagementSelfTest();
    if (EFI_ERROR(Status)) goto Cleanup;
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
    UiClearScreen();
    PrintPlatformSettings(gPlatform);
  }

  if (gAppDelaySeconds > 0) {
    if (!gAppQuietMode && SystemTable->ConIn) {
      BOOLEAN aborted = FALSE;
      for (UINT32 i = gAppDelaySeconds; i > 0; i--) {
        UiAsciiPrint("\rTime to exit: %u s... (Press ANY KEY to exit) ", i);
        
        for (UINTN j = 0; j < 100; j++) {
          EFI_STATUS KeyStatus = SystemTable->BootServices->CheckEvent(SystemTable->ConIn->WaitForKey);
          if (!EFI_ERROR(KeyStatus)) {
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
      SystemTable->BootServices->Stall((UINTN)gAppDelaySeconds * 1000000);
    }
  }

Cleanup:
  RemoveAllInterruptOverrides();
  if (EFI_ERROR(Status) && !gAppQuietMode) UiPrint(L"UnderVolter stopped: %r\n", Status);
  if (gPlatform) { FreePool(gPlatform); gPlatform = NULL; }
  SetMem(gCorePtrs, sizeof(gCorePtrs), 0);
  gNumCores = 0;
  ReleaseAppSettings();
  return Status;
}
