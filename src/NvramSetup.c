// NvramSetup.c — Read, patch, and write the UEFI NVRAM "Setup" variable.
//
// The "Setup" NVRAM EFI variable (GUID EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9)
// is a flat byte blob that the BIOS reads during POST to determine hardware
// configuration — including which MSR lock bits to assert:
//   offset 0x6ED: CFG Lock  (1 = locked → blocks MSR 0xE2 writes by OS/apps)
//   offset 0x789: OC Lock   (1 = locked → blocks MSR 0x150 OC Mailbox writes)
//
// Writing zero to these bytes before the next POST is functionally identical
// to grub-mod setup_var or RU.efi, but done entirely from within UnderVolter.
// Offsets are confirmed from the IFR dump of the Dell XPS 7590 BIOS v1.20.
//
// NOTE: Patches take effect on the NEXT BOOT only.  Lock bits for the current
// session were already set during POST and cannot be cleared at runtime.

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>

#include "NvramSetup.h"
#include "Config.h"
#include "UiConsole.h"

extern BOOLEAN gAppQuietMode;

// ─── Setup variable identity ──────────────────────────────────────────────────

// VarStore 0x1 from the IFR dump: variable name "Setup", size 0x17FD bytes.
// All hidden BIOS flags (CFG lock, OC lock, memory training, etc.) live here.
static EFI_GUID gSetupVarGuid = {
  0xEC87D643, 0xEBA4, 0x4BB5,
  { 0xA1, 0xE5, 0x3F, 0x3E, 0x36, 0xB2, 0x0D, 0xA9 }
};

// Cap on number of Patch_N entries accepted from the INI file.
#define NVRAM_MAX_PATCHES  16

// ─── Minimal string / hex helpers (no libc in UEFI pre-boot) ─────────────────

// Parse an unsigned hex integer from ASCII with optional "0x"/"0X" prefix.
// Stops at the first non-hex character.  Returns 0 and sets *OutValid=FALSE
// when no hex digits are found or the value exceeds 0xFFFF (max useful offset).
static UINT32 ParseHex(CONST CHAR8* s, BOOLEAN* OutValid) {
  *OutValid = FALSE;
  while (*s == ' ' || *s == '\t') s++;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  UINT32 v = 0;
  BOOLEAN hasDigit = FALSE;
  while (*s) {
    CHAR8 c = *s++;
    UINT32 nibble;
    if      (c >= '0' && c <= '9') nibble = (UINT32)(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = (UINT32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') nibble = (UINT32)(c - 'A' + 10);
    else break;
    v = v * 16 + nibble;
    hasDigit = TRUE;
    if (v > 0xFFFF) return 0;   // reject anything that won't fit in UINT16/UINT8
  }
  *OutValid = hasDigit;
  return v;
}

static UINTN StrLen8(CONST CHAR8* s) {
  UINTN n = 0; while (*s++) n++; return n;
}

// Case-insensitive prefix match: returns TRUE if s starts with lit (ASCII only).
// Does NOT advance s; caller must use strlen(lit) to skip past the matched part.
static BOOLEAN MatchCI(CONST CHAR8* s, CONST CHAR8* lit) {
  while (*lit) {
    CHAR8 a = *s, b = *lit;
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return FALSE;
    s++; lit++;
  }
  return TRUE;
}

// ─── [SetupVar] INI section parser ───────────────────────────────────────────

// Scan IniData for the [SetupVar] section and extract:
//   NvramPatchEnabled → *OutEnabled  (default FALSE if key absent)
//   NvramPatchReboot  → *OutReboot   (default TRUE  if key absent)
//   Patch_N = 0xOFFSET : 0xVALUE entries (N arbitrary, up to NVRAM_MAX_PATCHES)
// Returns the number of valid Patch_N entries found.
static UINTN ParseSetupVarSection(
  CONST CHAR8* IniData,
  BOOLEAN*     OutEnabled,
  BOOLEAN*     OutReboot,
  UINT16       OutOffsets[NVRAM_MAX_PATCHES],
  UINT8        OutValues [NVRAM_MAX_PATCHES]
) {
  *OutEnabled = FALSE;
  *OutReboot  = TRUE;

  if (!IniData) return 0;

  UINTN        count  = 0;
  CONST CHAR8* p      = IniData;
  BOOLEAN      inSect = FALSE;

  while (*p) {
    // Skip blank lines and leading whitespace
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) break;

    // Whole-line comments
    if (*p == ';' || *p == '#') { while (*p && *p != '\n') p++; continue; }

    if (*p == '[') {
      // Section header — check for [SetupVar] (case-insensitive, no spaces)
      p++;
      inSect = MatchCI(p, "SetupVar") && (p[8] == ']');
      while (*p && *p != '\n') p++;
      continue;
    }

    if (inSect) {
      BOOLEAN valid = FALSE;

      if (MatchCI(p, "NvramPatchEnabled")) {
        CONST CHAR8* eq = p + StrLen8("NvramPatchEnabled");
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq == '=') { eq++; *OutEnabled = (ParseHex(eq, &valid) != 0); }

      } else if (MatchCI(p, "NvramPatchReboot")) {
        CONST CHAR8* eq = p + StrLen8("NvramPatchReboot");
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq == '=') { eq++; *OutReboot = (ParseHex(eq, &valid) != 0); }

      } else if (MatchCI(p, "Patch_") && count < NVRAM_MAX_PATCHES) {
        // Format: Patch_N = 0xOFFSET : 0xVALUE
        // Both offset and value must parse as valid hex; reject silently if not.
        CONST CHAR8* eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq == '=') {
          eq++;
          BOOLEAN offValid = FALSE, valValid = FALSE;
          UINT32 offset = ParseHex(eq, &offValid);
          while (*eq && *eq != ':' && *eq != '\n') eq++;
          if (*eq == ':') {
            eq++;
            UINT32 val = ParseHex(eq, &valValid);
            if (offValid && valValid && val <= 0xFF) {
              OutOffsets[count] = (UINT16)offset;
              OutValues [count] = (UINT8)val;
              count++;
            }
          }
        }
      }
    }

    while (*p && *p != '\n') p++;
  }

  return count;
}

// ─── Public API ───────────────────────────────────────────────────────────────

VOID ApplyNvramSetupPatches(IN EFI_SYSTEM_TABLE* SystemTable)
{
  BOOLEAN enabled  = FALSE;
  BOOLEAN doReboot = TRUE;
  UINT16  offsets[NVRAM_MAX_PATCHES];
  UINT8   values [NVRAM_MAX_PATCHES];

  UINTN patchCount = ParseSetupVarSection(
    GetIniDataPtr(), &enabled, &doReboot, offsets, values);

  if (!enabled || patchCount == 0) return;

  EFI_RUNTIME_SERVICES* RT = SystemTable->RuntimeServices;

  // ── Query the size of the Setup variable ────────────────────────────────
  // First call with DataSize=0 returns EFI_BUFFER_TOO_SMALL and fills DataSize.
  UINTN      dataSize = 0;
  UINT32     attrs    = 0;
  EFI_STATUS status   = RT->GetVariable(
    L"Setup", &gSetupVarGuid, &attrs, &dataSize, NULL);

  if (status != EFI_BUFFER_TOO_SMALL || dataSize == 0) {
    if (!gAppQuietMode)
      UiPrint(L"[NVRAM] Cannot query Setup variable size (0x%x)\n", status);
    return;
  }

  // ── Read the full variable ───────────────────────────────────────────────
  UINT8* data = AllocatePool(dataSize);
  if (!data) {
    if (!gAppQuietMode)
      UiPrint(L"[NVRAM] AllocatePool failed (%u bytes)\n", (UINT32)dataSize);
    return;
  }

  status = RT->GetVariable(L"Setup", &gSetupVarGuid, &attrs, &dataSize, data);
  if (EFI_ERROR(status)) {
    if (!gAppQuietMode)
      UiPrint(L"[NVRAM] GetVariable failed (0x%x)\n", status);
    FreePool(data);
    return;
  }

  // ── Compare current values against desired — skip bytes already correct ──
  // This prevents an infinite reboot loop: if all offsets already hold the
  // target values (e.g. after a successful patch+reboot), nothing is written
  // and no reboot is triggered.
  UINTN applied = 0;
  for (UINTN i = 0; i < patchCount; i++) {
    UINT16 off = offsets[i];
    UINT8  val = values[i];

    if ((UINTN)off >= dataSize) {
      if (!gAppQuietMode)
        UiPrint(L"[NVRAM]   [0x%03X] SKIP (offset >= variable size)\n", off);
      continue;
    }

    if (data[off] == val) {
      // Already the desired value — no write needed for this offset.
      if (!gAppQuietMode)
        UiPrint(L"[NVRAM]   [0x%03X]  0x%02X (already set)\n", off, val);
      continue;
    }

    if (!gAppQuietMode)
      UiPrint(L"[NVRAM]   [0x%03X]  0x%02X -> 0x%02X\n", off, data[off], val);
    data[off] = val;
    applied++;
  }

  if (applied == 0) {
    // All bytes already match — nothing to write, no reboot needed.
    if (!gAppQuietMode)
      UiPrint(L"[NVRAM] All offsets already set correctly — no action needed.\n");
    FreePool(data);
    return;
  }

  if (!gAppQuietMode)
    UiPrint(L"[NVRAM] Setup variable: %u bytes, %u byte(s) changed:\n",
            (UINT32)dataSize, (UINT32)applied);

  // ── Write the patched variable back ─────────────────────────────────────
  // Use the same attributes read from GetVariable to preserve NV/BS/RT flags.
  status = RT->SetVariable(L"Setup", &gSetupVarGuid, attrs, dataSize, data);
  FreePool(data);

  if (EFI_ERROR(status)) {
    if (!gAppQuietMode)
      UiPrint(L"[NVRAM] SetVariable failed (0x%x) — patches NOT written.\n", status);
    return;
  }

  // ── Verify the write by reading back ────────────────────────────────────
  // Some firmware silently ignores SetVariable (write-protected NVRAM) while
  // returning EFI_SUCCESS.  A read-back in the same session catches this before
  // we trigger a reboot that would lead to an infinite restart loop.
  // Verification is fail-closed: any failure to read back (alloc error, API
  // error, offset out of range) is treated as unverified — no reboot.
  {
    UINTN  verifySize  = 0;
    UINT32 verifyAttrs = 0;
    BOOLEAN verified   = FALSE;

    status = RT->GetVariable(
      L"Setup", &gSetupVarGuid, &verifyAttrs, &verifySize, NULL);

    if (status == EFI_BUFFER_TOO_SMALL && verifySize > 0) {
      UINT8* verify = AllocatePool(verifySize);
      if (verify) {
        status = RT->GetVariable(
          L"Setup", &gSetupVarGuid, &verifyAttrs, &verifySize, verify);
        if (!EFI_ERROR(status)) {
          UINTN mismatch = 0;
          for (UINTN i = 0; i < patchCount; i++) {
            UINT16 off = offsets[i];
            // Offset out of range counts as mismatch — patch could not land.
            if ((UINTN)off >= verifySize) {
              mismatch++;
              continue;
            }
            if (verify[off] != values[i])
              mismatch++;
          }
          if (mismatch == 0)
            verified = TRUE;
          else if (!gAppQuietMode)
            UiPrint(L"[NVRAM] Verify failed: %u offset(s) did not stick"
                    L" — firmware may be write-protecting this variable.\n",
                    (UINT32)mismatch);
        } else {
          if (!gAppQuietMode)
            UiPrint(L"[NVRAM] Verify read-back failed (0x%x) — reboot aborted.\n",
                    status);
        }
        FreePool(verify);
      } else {
        if (!gAppQuietMode)
          UiPrint(L"[NVRAM] Verify AllocatePool failed — reboot aborted.\n");
      }
    } else {
      if (!gAppQuietMode)
        UiPrint(L"[NVRAM] Verify size probe failed (0x%x) — reboot aborted.\n",
                status);
    }

    if (!verified) return;   // do not reboot — write could not be confirmed
  }

  if (!gAppQuietMode)
    UiPrint(L"[NVRAM] Write verified. Changes activate on next boot.\n");

  // ── Reboot countdown ─────────────────────────────────────────────────────
  if (!doReboot) {
    if (!gAppQuietMode)
      UiPrint(L"[NVRAM] NvramPatchReboot = 0 — reboot manually to activate.\n");
    return;
  }

  if (!gAppQuietMode) {
    // 3-second cancellable countdown; any key aborts the reboot
    for (UINT32 sec = 3; sec > 0; sec--) {
      UiPrint(L"\r[NVRAM] Rebooting in %u s... (any key to cancel)  ", sec);
      for (UINTN j = 0; j < 100; j++) {
        EFI_STATUS ev = SystemTable->BootServices->CheckEvent(
          SystemTable->ConIn->WaitForKey);
        if (!EFI_ERROR(ev)) {
          EFI_INPUT_KEY key = { 0, 0 };
          SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key);
          UiPrint(L"\n[NVRAM] Reboot cancelled — reboot manually to activate.\n");
          return;
        }
        SystemTable->BootServices->Stall(10000);   // 10 ms
      }
    }
    UiPrint(L"\n");
  }

  RT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
  // Not reached after warm reset.
}
