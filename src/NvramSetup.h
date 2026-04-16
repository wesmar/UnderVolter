// NvramSetup.h — UEFI NVRAM "Setup" variable patcher.
//                Reads [SetupVar] from UnderVolter.ini and applies byte-level
//                patches to the BIOS configuration variable before CPU programming.
#pragma once

#include <Uefi.h>

// Apply NVRAM Setup variable patches from the [SetupVar] INI section.
// Must be called after LoadAppSettings() and UiConsoleInit() while boot services
// are still active.
//
// If NvramPatchEnabled = 1: reads the UEFI "Setup" EFI variable, patches the
// bytes at the listed offsets, and writes the variable back.  If NvramPatchReboot
// is also 1, a 3-second countdown is shown and then a warm reset is issued —
// this function does not return in that case.  Patches only activate on the next
// POST cycle; they have no effect on the currently running session.
//
// Has no effect when NvramPatchEnabled = 0 or no Patch_N entries are present.
VOID ApplyNvramSetupPatches(IN EFI_SYSTEM_TABLE* SystemTable);
