// MpDispatcher.h — MP dispatch helpers: run a callback on a specific CPU (AP or BSP)
//                  or on all processors concurrently/serially via EFI_MP_SERVICES_PROTOCOL.
#pragma once

#include "Platform.h"

// Run proc on the CPU identified by CpuNumber; falls back to in-place execution
// on BSP if MP services are unavailable or CpuNumber == BootProcessor.
EFI_STATUS EFIAPI RunOnPackageOrCore(
  const IN PLATFORM *Platform,
  const IN UINTN CpuNumber,
  const IN EFI_AP_PROCEDURE proc,
  IN VOID *param OPTIONAL
);

// Run proc on every logical CPU.  runConcurrent=TRUE launches all APs at once
// and waits for an EFI_EVENT signal; FALSE uses StartupAllAPs serialised mode.
EFI_STATUS EFIAPI RunOnAllProcessors(
  const IN EFI_AP_PROCEDURE proc,
  const BOOLEAN runConcurrent,                  // FALSE = serial execution
  IN VOID *param OPTIONAL
);
