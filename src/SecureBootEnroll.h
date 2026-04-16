// SecureBootEnroll.h — Secure Boot key enrollment from .auth files on the ESP.
//                      Reads [SecureBoot] section from UnderVolter.ini and
//                      writes PK / KEK / db / dbx via SetVariable with
//                      time-based authenticated write access.
#pragma once

#include <Uefi.h>

// Enroll Secure Boot keys from .auth files located in [SecureBoot] KeyDir.
// Must be called after LoadAppSettings() while Boot Services are still active.
// No-op when SecureBootEnroll = 0 (default) or no .auth files are present.
VOID EnrollSecureBootKeys(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
);
