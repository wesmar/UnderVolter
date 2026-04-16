// Config.c — INI-file loader and policy mapper: reads UnderVolter.ini from the
//            ESP (tries loader directory, OC paths, then all volumes), parses
//            per-domain voltage/IccMax/VF-point keys, and populates PACKAGE structs.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Library/DevicePathLib.h>

#include "Platform.h"
#include "Config.h"
#include "CpuData.h"
#include "UiConsole.h"

UINT8 gPostProgrammingOcLock = 1;
UINT8 gEmergencyExit = 1;
UINT8 gEnableSaferAsm = 1;
UINT8 gDisableFirmwareWDT = 0;
UINT64 gSelfTestMaxRuns = 0;
UINT8 gPrintPackageConfig = 1;
UINT8 gPrintVFPoints_PostProgram = 1;

extern EFI_HANDLE gImageHandle;

static CHAR8*   gIniData   = NULL;
static BOOLEAN  gIniLoaded = FALSE;

CONST CHAR8* GetIniDataPtr(VOID) { return gIniData; }

// ─── Minimal string helpers (no libc available in UEFI pre-boot) ─────────────

// Case-sensitive bounded comparison; returns <0/0/>0 like strncmp.
static INTN StrnCmpAscii(CONST CHAR8* s1, CONST CHAR8* s2, UINTN n) {
  if (n == 0) return 0;
  while (n-- > 0 && *s1 && (*s1 == *s2)) {
    if (n == 0) return 0;
    s1++;
    s2++;
  }
  return *(CONST INT8*)s1 - *(CONST INT8*)s2;
}

// Case-insensitive bounded comparison (ASCII only, -32 uppercasing trick).
static INTN StrniCmpAscii(CONST CHAR8* s1, CONST CHAR8* s2, UINTN n) {
  if (n == 0) return 0;
  while (n-- > 0) {
    CHAR8 c1 = (*s1 >= 'a' && *s1 <= 'z') ? *s1 - 32 : *s1;
    CHAR8 c2 = (*s2 >= 'a' && *s2 <= 'z') ? *s2 - 32 : *s2;
    if (c1 != c2) return c1 - c2;
    if (c1 == '\0') return 0;
    s1++;
    s2++;
  }
  return 0;
}

static UINTN StrLenAscii(CONST CHAR8* s) {
  UINTN len = 0;
  while (*s++) len++;
  return len;
}

// Absolute value for INT16; avoids dependency on abs() from libc.
static INT16 AbsI16(INT16 v) {
  return (v < 0) ? (INT16)-v : v;
}

static BOOLEAN StriEqualsAscii(CONST CHAR8* a, CONST CHAR8* b) {
  UINTN la = StrLenAscii(a);
  UINTN lb = StrLenAscii(b);
  if (la != lb) return FALSE;
  return (StrniCmpAscii(a, b, la) == 0);
}

// Parse a decimal integer (with optional leading sign) from an ASCII string.
static INT64 ParseInt(CONST CHAR8* str) {
  INT64 result = 0;
  BOOLEAN neg = FALSE;
  while (*str == ' ' || *str == '\t') str++;
  if (*str == '-') { neg = TRUE; str++; }
  while (*str >= '0' && *str <= '9') {
    result = result * 10 + (*str - '0');
    str++;
  }
  return neg ? -result : result;
}

// Locate the value string for Key inside [Section] in an INI buffer.
// Returns a pointer to the first character after '=' (trimmed), or NULL if not found.
// Handles ';' and '#' comment lines; section match is case-sensitive.
static CHAR8* IniFindValue(CHAR8* IniData, CONST CHAR8* Section, CONST CHAR8* Key) {
  CHAR8* p = IniData;
  BOOLEAN inSection = FALSE;
  UINTN secLen = StrLenAscii(Section);
  UINTN keyLen = StrLenAscii(Key);
  
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '\0') break;
    
    if (*p == ';' || *p == '#') {
      while (*p && *p != '\n') p++;
      continue;
    }
    
    if (*p == '[') {
      p++;
      if (StrnCmpAscii(p, Section, secLen) == 0 && p[secLen] == ']') {
        inSection = TRUE;
      } else {
        inSection = FALSE;
      }
      while (*p && *p != '\n') p++;
      continue;
    }
    
    if (inSection) {
      if (StrnCmpAscii(p, Key, keyLen) == 0) {
        CHAR8* k = p + keyLen;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '=') {
          k++;
          while (*k == ' ' || *k == '\t') k++;
          return k;
        }
      }
    }
    
    while (*p && *p != '\n') p++;
  }
  return NULL;
}

static UINT32 IniGetInt(CHAR8* IniData, CONST CHAR8* Section, CONST CHAR8* Key, UINT32 DefaultValue) {
  CHAR8* val = IniFindValue(IniData, Section, Key);
  if (val) {
      return (UINT32)ParseInt(val);
  }
  return DefaultValue;
}


static BOOLEAN IniTryGetUInt(CHAR8* IniData, CONST CHAR8* Section, CONST CHAR8* Key, UINT32* OutValue) {
  CHAR8* val = IniFindValue(IniData, Section, Key);
  if (!val) return FALSE;
  *OutValue = (UINT32)ParseInt(val);
  return TRUE;
}

static BOOLEAN IniTryGetInt(CHAR8* IniData, CONST CHAR8* Section, CONST CHAR8* Key, INT32* OutValue) {
  CHAR8* val = IniFindValue(IniData, Section, Key);
  if (!val) return FALSE;
  *OutValue = (INT32)ParseInt(val);
  return TRUE;
}

// Search for the first INI section that contains "Architecture = ArchName".
// On match, writes the section name into OutSection (caller-allocated, >=64 bytes).
// Used to pick the microarchitecture-specific profile block (e.g. [Profile.ADL]).
static BOOLEAN FindProfileSection(CHAR8* IniData, CONST CHAR8* ArchName, CHAR8* OutSection) {
  CHAR8* p = IniData;
  CHAR8 secName[64];
  BOOLEAN inSection = FALSE;

  secName[0] = '\0';

  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '\0') break;

    if (*p == ';' || *p == '#') {
      while (*p && *p != '\n') p++;
      continue;
    }

    if (*p == '[') {
      p++;
      UINTN i = 0;
      while (*p && *p != ']' && *p != '\n' && i < 63) {
        secName[i++] = *p++;
      }
      secName[i] = '\0';
      inSection = TRUE;
      while (*p && *p != '\n') p++;
      continue;
    }

    if (inSection) {
      CHAR8* line = p;
      while (*line == ' ' || *line == '\t') line++;

      CONST CHAR8* key = "Architecture";
      UINTN keyLen = StrLenAscii(key);
      if (StrnCmpAscii(line, key, keyLen) == 0) {
        CHAR8* k = line + keyLen;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '=') {
          k++;
          while (*k == ' ' || *k == '\t') k++;

          CHAR8 cleanArch[64];
          UINTN j = 0;
          while (*k && *k != '\r' && *k != '\n' && j < 63) {
            if (*k != '"' && *k != '\'') {
              cleanArch[j++] = *k;
            }
            k++;
          }
          cleanArch[j] = '\0';

          if (StriEqualsAscii(cleanArch, ArchName)) {
            UINTN j2 = 0;
            while (secName[j2]) { OutSection[j2] = secName[j2]; j2++; }
            OutSection[j2] = '\0';
            return TRUE;
          }
        }
      }
    }

    while (*p && *p != '\n') p++;
  }

  return FALSE;
}

// Derive the expected INI path by replacing the loaded EFI's filename with
// "UnderVolter.ini" in the same directory.  Falls back to manual device-path
// node parsing when ConvertDevicePathToText is unavailable.
static CHAR16* GetIniPathFromLoadedImage(EFI_LOADED_IMAGE_PROTOCOL *LoadedImage) {
  EFI_DEVICE_PATH_PROTOCOL *Dp = (EFI_DEVICE_PATH_PROTOCOL *)LoadedImage->FilePath;
  if (Dp == NULL) return NULL;

  // Try to use library for full path conversion if available
  CHAR16* FullPath = ConvertDevicePathToText(Dp, FALSE, FALSE);
  if (FullPath == NULL) {
    // Manual fallback parsing in case protocol is missing or fails
    // Handle multi-node file paths by concatenating them
    UINTN TotalLen = 0;
    EFI_DEVICE_PATH_PROTOCOL *Node = Dp;
    while (!IsDevicePathEnd(Node)) {
      if (DevicePathType(Node) == MEDIA_DEVICE_PATH && DevicePathSubType(Node) == MEDIA_FILEPATH_DP) {
        TotalLen += (DevicePathNodeLength(Node) - 4) / 2;
      }
      Node = NextDevicePathNode(Node);
    }
    
    if (TotalLen == 0) return NULL;
    
    EFI_STATUS Status = gBS->AllocatePool(EfiBootServicesData, (TotalLen + 32) * sizeof(CHAR16), (VOID**)&FullPath);
    if (EFI_ERROR(Status) || !FullPath) return NULL;
    
    UINTN CurrentPos = 0;
    Node = Dp;
    while (!IsDevicePathEnd(Node)) {
      if (DevicePathType(Node) == MEDIA_DEVICE_PATH && DevicePathSubType(Node) == MEDIA_FILEPATH_DP) {
        FILEPATH_DEVICE_PATH *Fp = (FILEPATH_DEVICE_PATH *)Node;
        UINTN NodeCharCount = (DevicePathNodeLength(Node) - 4) / 2;
        for (UINTN i = 0; i < NodeCharCount; i++) {
          if (Fp->PathName[i] == L'\0') break;
          FullPath[CurrentPos++] = Fp->PathName[i];
        }
      }
      Node = NextDevicePathNode(Node);
    }
    FullPath[CurrentPos] = L'\0';
  }

  // Find last backslash to swap filename
  UINTN PathLen = 0;
  while (FullPath[PathLen]) PathLen++;

  INTN LastSlash = (INTN)PathLen - 1;
  while (LastSlash >= 0 && FullPath[LastSlash] != L'\\') {
    LastSlash--;
  }

  UINTN InsertPos = (LastSlash >= 0) ? (UINTN)LastSlash + 1 : 0;
  
  // Create final path
  CHAR16* NewPath = NULL;
  CONST CHAR16* IniName = L"UnderVolter.ini";
  UINTN IniNameLen = 15;
  
  EFI_STATUS Status = gBS->AllocatePool(EfiBootServicesData, (InsertPos + IniNameLen + 1) * sizeof(CHAR16), (VOID**)&NewPath);
  if (!EFI_ERROR(Status) && NewPath) {
    for (UINTN i = 0; i < InsertPos; i++) NewPath[i] = FullPath[i];
    for (UINTN i = 0; i < IniNameLen; i++) NewPath[InsertPos + i] = IniName[i];
    NewPath[InsertPos + IniNameLen] = L'\0';
  }

  gBS->FreePool(FullPath);
  return NewPath;
}

// Known INI locations tried when the dynamic path lookup doesn't work.
// Ordered by likelihood:
//   1. OpenCore (OC) — Drivers / Tools / root
//   2. Microsoft Boot Manager — next to bootmgfw.efi
//   3. Generic EFI locations — \EFI\Boot, \EFI, volume root
static const CHAR16* kIniFallbackPaths[] = {
  // OpenCore
  L"\\EFI\\OC\\Drivers\\UnderVolter.ini",
  L"\\EFI\\OC\\Tools\\UnderVolter.ini",
  L"\\EFI\\OC\\UnderVolter.ini",
  // Microsoft Boot Manager
  L"\\EFI\\Microsoft\\Boot\\UnderVolter.ini",
  // Generic EFI locations
  L"\\EFI\\Boot\\UnderVolter.ini",
  L"\\EFI\\UnderVolter.ini",
  // ESP root
  L"\\UnderVolter.ini",
};
#define INI_FALLBACK_COUNT (sizeof(kIniFallbackPaths) / sizeof(kIniFallbackPaths[0]))

// Try to open UnderVolter.ini on the given volume root.
// Tries the dynamic path (derived from LoadedImage path) first, then fallbacks.
static BOOLEAN TryOpenIniOnRoot(EFI_FILE_PROTOCOL* Root,
                                 CHAR16*            DynPath,
                                 EFI_FILE_PROTOCOL** OutFile)
{
  if (DynPath &&
      !EFI_ERROR(Root->Open(Root, OutFile, DynPath, EFI_FILE_MODE_READ, 0))) {
    return TRUE;
  }
  for (UINTN i = 0; i < INI_FALLBACK_COUNT; i++) {
    if (!EFI_ERROR(Root->Open(Root, OutFile, (CHAR16*)kIniFallbackPaths[i],
                              EFI_FILE_MODE_READ, 0))) {
      return TRUE;
    }
  }
  return FALSE;
}

static EFI_STATUS ReadIniFile(CHAR8** OutData, UINTN* OutSize) {
  EFI_STATUS Status;
  EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
  EFI_FILE_PROTOCOL* Root = NULL;
  EFI_FILE_PROTOCOL* File = NULL;

  Status = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid,
                                (VOID**)&LoadedImage);
  if (EFI_ERROR(Status)) return Status;

  CHAR16* DynamicPath = GetIniPathFromLoadedImage(LoadedImage);

  // ── Try 1: SimpleFileSystem directly on LoadedImage->DeviceHandle ────────────
  // Works in QEMU and on standard UEFI firmware that maps the partition handle
  // correctly as the device handle.
  {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
    if (!EFI_ERROR(gBS->HandleProtocol(LoadedImage->DeviceHandle,
                                        &gEfiSimpleFileSystemProtocolGuid,
                                        (VOID**)&Fs)) &&
        !EFI_ERROR(Fs->OpenVolume(Fs, &Root))) {
      if (TryOpenIniOnRoot(Root, DynamicPath, &File)) {
        goto got_file;
      }
      Root->Close(Root);
      Root = NULL;
    }
  }

  // ── Try 2: Search ALL SimpleFileSystem handles ────────────────────────────────
  // OpenCore commonly sets LoadedImage->DeviceHandle to a controller/disk handle
  // that has no SimpleFileSystem — the partition handle (with the FS) is a child.
  // LocateHandleBuffer finds all mounted volumes so we can try each one.
  {
    UINTN HandleCount = 0;
    EFI_HANDLE* HandleBuffer = NULL;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,
                                            &gEfiSimpleFileSystemProtocolGuid,
                                            NULL, &HandleCount, &HandleBuffer))) {
      for (UINTN hi = 0; hi < HandleCount; hi++) {
        if (HandleBuffer[hi] == LoadedImage->DeviceHandle) {
          continue;   // already tried in step 1
        }
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(HandleBuffer[hi],
                                           &gEfiSimpleFileSystemProtocolGuid,
                                           (VOID**)&Fs))) {
          continue;
        }
        EFI_FILE_PROTOCOL* Rt = NULL;
        if (EFI_ERROR(Fs->OpenVolume(Fs, &Rt))) {
          continue;
        }
        if (TryOpenIniOnRoot(Rt, DynamicPath, &File)) {
          Root = Rt;
          gBS->FreePool(HandleBuffer);
          goto got_file;
        }
        Rt->Close(Rt);
      }
      gBS->FreePool(HandleBuffer);
    }
  }

  if (DynamicPath) gBS->FreePool(DynamicPath);
  return EFI_NOT_FOUND;

got_file:
  if (DynamicPath) gBS->FreePool(DynamicPath);

  EFI_FILE_INFO *FileInfo = NULL;
  UINTN BufferSize = 0;
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &BufferSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Status = gBS->AllocatePool(EfiBootServicesData, BufferSize, (VOID**)&FileInfo);
    if (!EFI_ERROR(Status)) {
      Status = File->GetInfo(File, &gEfiFileInfoGuid, &BufferSize, FileInfo);
    }
  }

  if (EFI_ERROR(Status) || FileInfo == NULL) {
    File->Close(File);
    Root->Close(Root);
    if (FileInfo) gBS->FreePool(FileInfo);
    return EFI_DEVICE_ERROR;
  }

  UINTN FileSize = (UINTN)FileInfo->FileSize;
  gBS->FreePool(FileInfo);

  CHAR8* Buffer = NULL;
  Status = gBS->AllocatePool(EfiBootServicesData, FileSize + 1, (VOID**)&Buffer);
  if (EFI_ERROR(Status)) {
    File->Close(File);
    Root->Close(Root);
    return Status;
  }

  BufferSize = FileSize;
  Status = File->Read(File, &BufferSize, Buffer);
  if (EFI_ERROR(Status)) {
    gBS->FreePool(Buffer);
    File->Close(File);
    Root->Close(Root);
    return Status;
  }

  Buffer[FileSize] = '\0';
  *OutData = Buffer;
  *OutSize = FileSize;

  File->Close(File);
  Root->Close(Root);
  return EFI_SUCCESS;
}

// Construct a composite INI key like "VF_Point_3_IACORE" into Buffer.
// Prefix + decimal(Index) + Suffix are concatenated without heap allocation.
static VOID BuildKey(CHAR8* Buffer, CONST CHAR8* Prefix, UINTN Index, CONST CHAR8* Suffix) {
  while (*Prefix) *Buffer++ = *Prefix++;
  CHAR8 numStr[20];
  
  UINTN i = 0;
  UINTN Value = Index;
  if (Value == 0) {
    numStr[i++] = '0';
  } else {
    while (Value > 0) {
      numStr[i++] = (CHAR8)((Value % 10) + '0');
      Value /= 10;
    }
  }
  
  while (i > 0) {
    *Buffer++ = numStr[--i];
  }
  
  while (*Suffix) *Buffer++ = *Suffix++;
  *Buffer = '\0';
}

// Map a frequency in MHz to the matching VF point index in dom->vfPoint[].
// Converts MHz → kHz → ratio using gBCLK_bsp (BCLK in kHz), then scans for
// a fused ratio match.  Returns -1 if no point matches.
static INTN FindVfPointIndexByFreq(DOMAIN* dom, INT64 freqMHz) {
  if (!dom || freqMHz <= 0 || gBCLK_bsp == 0) return -1;

  INT64 freqKhz = freqMHz * 1000;
  INT64 ratio64 = (freqKhz + ((INT64)gBCLK_bsp / 2)) / (INT64)gBCLK_bsp;
  if (ratio64 <= 0 || ratio64 > 255) return -1;

  UINT8 ratio = (UINT8)ratio64;

  for (UINTN i = 0; i < dom->nVfPoints; i++) {
    if (dom->vfPoint[i].IsValid && dom->vfPoint[i].FusedRatio == ratio) {
      return (INTN)i;
    }
  }

  return -1;
}

// Read all voltage/IccMax/VF-point keys for one domain from the INI section Sec
// and populate the corresponding PACKAGE fields.  DomName is the domain suffix
// used in key names (e.g. "IACORE", "RING").  Sets Program_VF_Overrides[domIdx]
// if any non-default value was found.
static VOID SetDomainSettings(CHAR8* IniData, CONST CHAR8* Sec, PACKAGE* pk, UINT8 domIdx, CONST CHAR8* DomName) {
  CHAR8 key[64];
  UINT32 uval = 0;
  INT32 sval = 0;
  BOOLEAN vfKeyPresent = FALSE;
  BOOLEAN vfPointFound = FALSE;
  BOOLEAN vfPointApplied = FALSE;
  BOOLEAN anyOverrides = FALSE;
  
  // IccMax
  key[0] = '\0';
  UINTN i = 0;
  CONST CHAR8* p = "IccMax_"; while (*p) key[i++] = *p++;
  CONST CHAR8* p2 = DomName; while (*p2) key[i++] = *p2++;
  key[i] = '\0';
  
  if (IniTryGetUInt(IniData, Sec, key, &uval)) {
    pk->Program_IccMax[domIdx] = 1;
    pk->planes[domIdx].IccMax = (UINT16)uval;
  }

  // OffsetVolts
  i = 0;
  p = "OffsetVolts_"; while (*p) key[i++] = *p++;
  p2 = DomName; while (*p2) key[i++] = *p2++;
  key[i] = '\0';
  
  if (IniTryGetInt(IniData, Sec, key, &sval)) {
    pk->planes[domIdx].VoltMode = V_IPOLATIVE;
    pk->planes[domIdx].OffsetVolts = (INT16)sval;
  }

  // TargetVolts
  i = 0;
  p = "TargetVolts_"; while (*p) key[i++] = *p++;
  p2 = DomName; while (*p2) key[i++] = *p2++;
  key[i] = '\0';
  
  if (IniTryGetUInt(IniData, Sec, key, &uval)) {
    pk->planes[domIdx].TargetVolts = (UINT16)uval;
  }

  // Program_VF_Points
  i = 0;
  p = "Program_VF_Points_"; while (*p) key[i++] = *p++;
  p2 = DomName; while (*p2) key[i++] = *p2++;
  key[i] = '\0';
  
  if (IniTryGetUInt(IniData, Sec, key, &uval)) {
    vfKeyPresent = TRUE;
    pk->Program_VF_Points[domIdx] = (UINT8)uval;
    if (pk->Program_VF_Points[domIdx]) {
      anyOverrides = TRUE;
    }
  }

  // VF Points
  for (UINTN v = 0; v < MAX_VF_POINTS; v++) {
    CHAR8 vfKey[64];
    BuildKey(vfKey, "VF_Point_", v, "_");
    
    i = 0; while (vfKey[i]) i++;
    p2 = DomName; while (*p2) vfKey[i++] = *p2++;
    vfKey[i] = '\0';
    
    CHAR8* val = IniFindValue(IniData, Sec, vfKey);
    if (val) {
      vfPointFound = TRUE;
      // Key format: "VF_Point_N_DOMNAME = Freq_MHz:Offset_mV"
      INT64 freq = ParseInt(val);
      while (*val && *val != ':') val++;
      if (*val == ':') {
        val++;
        INT64 off = ParseInt(val);

        INTN idx = FindVfPointIndexByFreq(&pk->planes[domIdx], freq);
        if (idx >= 0) {
          pk->planes[domIdx].vfPoint[idx].VOffset = (INT16)off;
          vfPointApplied = TRUE;
        } else {
          if (!gAppQuietMode) {
            UiAsciiPrint("WARNING: VF_Point_%u_%a=%d MHz did not match any fused V/F ratio.\n",
              (UINT32)v, DomName, (INT32)freq);
          }
        }
      } else if (!gAppQuietMode) {
        UiAsciiPrint("WARNING: VF_Point_%u_%a missing ':' separator. Use Freq_MHz:Offset_mV\n",
          (UINT32)v, DomName);
      }
    }
  }

  if (vfPointApplied) {
    anyOverrides = TRUE;
    if (!vfKeyPresent) {
      pk->Program_VF_Points[domIdx] = 1;
    } else if ((pk->Program_VF_Points[domIdx] != 1) && !gAppQuietMode) {
      UiAsciiPrint("WARNING: VF_Point entries found but Program_VF_Points_%a=%u (not 1).\n",
        DomName, pk->Program_VF_Points[domIdx]);
    }
  } else if (vfPointFound && !gAppQuietMode) {
    UiAsciiPrint("WARNING: VF_Point entries found for %a but none matched fused V/F ratios.\n",
      DomName);
  }

  if (pk->planes[domIdx].OffsetVolts != 0 || pk->planes[domIdx].TargetVolts != 0) {
    anyOverrides = TRUE;
  }

  pk->Program_VF_Overrides[domIdx] = anyOverrides ? 1 : 0;
}

UINT32 gAppDelaySeconds = 0;
BOOLEAN gAppQuietMode = FALSE;
BOOLEAN gIniFound = FALSE;

// Idempotent loader: reads and caches the INI file on first call; subsequent
// calls return immediately.  gIniFound is set only when a valid file was read.
static VOID EnsureIniLoaded(VOID)
{
  if (gIniLoaded) {
    return;
  }

  gIniLoaded = TRUE;

  UINTN IniSize = 0;
  EFI_STATUS Status = ReadIniFile(&gIniData, &IniSize);
  if (EFI_ERROR(Status) || !gIniData) {
    gIniFound = FALSE;
    gIniData = NULL;
    return;
  }

  gIniFound = TRUE;
}

VOID LoadAppSettings(VOID)
{
  EnsureIniLoaded();

  if (!gIniData) {
    gAppDelaySeconds = 0;
    gAppQuietMode = FALSE;
    return;
  }

  gAppDelaySeconds = (UINT32)IniGetInt(gIniData, "Global", "DelaySeconds", 0);
  gAppQuietMode = (BOOLEAN)IniGetInt(gIniData, "Global", "QuietMode", 0);

  CHAR8* val = NULL;

  val = IniFindValue(gIniData, "Global", "PostProgrammingOcLock");
  if (val) gPostProgrammingOcLock = (UINT8)ParseInt(val);

  val = IniFindValue(gIniData, "Global", "EmergencyExit");
  if (val) gEmergencyExit = (UINT8)ParseInt(val);

  val = IniFindValue(gIniData, "Global", "EnableSaferAsm");
  if (val) gEnableSaferAsm = (UINT8)ParseInt(val);

  val = IniFindValue(gIniData, "Global", "DisableFirmwareWDT");
  if (val) {
    gDisableFirmwareWDT = (UINT8)ParseInt(val);
  } else {
    val = IniFindValue(gIniData, "Global", "DisableFirmwareWDT");
    if (val) gDisableFirmwareWDT = (UINT8)ParseInt(val);
  }

  val = IniFindValue(gIniData, "Global", "SelfTestMaxRuns");
  if (val) gSelfTestMaxRuns = (UINT64)ParseInt(val);

  val = IniFindValue(gIniData, "Global", "PrintPackageConfig");
  if (val) gPrintPackageConfig = (UINT8)ParseInt(val);

  val = IniFindValue(gIniData, "Global", "PrintVFPoints_PostProgram");
  if (val) gPrintVFPoints_PostProgram = (UINT8)ParseInt(val);
}

VOID ReleaseAppSettings(VOID)
{
  if (gIniData) {
    gBS->FreePool(gIniData);
  }
  gIniData   = NULL;
  gIniLoaded = FALSE;
  gIniFound = FALSE;
}

// Read all policy keys from the INI file and populate every PACKAGE in sys.
// If no INI is found, all programming flags remain at safe (disabled) defaults.
VOID ApplyComputerOwnersPolicy(IN PLATFORM* sys)
{
  EnsureIniLoaded();
  CHAR8* IniData = gIniData;

  if (!IniData) {
    if (!gAppQuietMode) {
      UiAsciiPrint("WARNING: UnderVolter.ini not found! Using safe fallback defaults.\n");
    }
  } else {
    if (!gAppQuietMode) {
      UiAsciiPrint("Parsed UnderVolter.ini successfully.\n");
    }
  }

  for (UINTN pidx = 0; pidx < sys->PkgCnt; pidx++) {
    PACKAGE* pk = sys->packages + pidx;
    
    // Safest fallbacks in case INI is not present
    pk->ProgramPowerTweaks = 0;
    pk->EnableEETurbo = 0;
    pk->EnableRaceToHalt = 0;
    
    for(UINT8 d = 0; d < MAX_DOMAINS; d++) {
        pk->planes[d].VoltMode = V_IPOLATIVE;
        pk->planes[d].TargetVolts = 0;
        pk->planes[d].OffsetVolts = 0;
        pk->Program_IccMax[d] = 0;
        pk->Program_VF_Points[d] = 0;
        pk->Program_VF_Overrides[d] = 0;
    }

    if (IniData) {
      // Globals
      pk->ProgramPowerTweaks = (UINT8)IniGetInt(IniData, "Global", "ProgramPowerTweaks", pk->ProgramPowerTweaks);
      pk->EnableEETurbo = (UINT8)IniGetInt(IniData, "Global", "EnableEETurbo", pk->EnableEETurbo);
      pk->EnableRaceToHalt = (UINT8)IniGetInt(IniData, "Global", "EnableRaceToHalt", pk->EnableRaceToHalt);
      pk->MaxCTDPLevel = (UINT8)IniGetInt(IniData, "Global", "MaxCTDPLevel", 0);
      pk->TdpControLock = (UINT8)IniGetInt(IniData, "Global", "TdpControlLock", 0);

      // Find Profile
      CHAR8 Sec[64] = "Profile.Generic";
      if (gActiveCpuData && gActiveCpuData->uArch[0] != '\0') {
        if (!FindProfileSection(IniData, gActiveCpuData->uArch, Sec)) {
          if (!gAppQuietMode) {
            UiAsciiPrint("WARNING: No profile found for %a. Searching for default.\n", gActiveCpuData->uArch);
          }
        }
      }

      // Ratios
      pk->ForcedRatioForPCoreCounts = (UINT8)IniGetInt(IniData, Sec, "ForcedRatioForPCoreCounts", 0);
      pk->ForcedRatioForECoreCounts = (UINT8)IniGetInt(IniData, Sec, "ForcedRatioForECoreCounts", 0);

      // Domains
      SetDomainSettings(IniData, Sec, pk, IACORE, "IACORE");
      SetDomainSettings(IniData, Sec, pk, RING, "RING");
      SetDomainSettings(IniData, Sec, pk, UNCORE, "UNCORE");
      SetDomainSettings(IniData, Sec, pk, GTSLICE, "GTSLICE");
      SetDomainSettings(IniData, Sec, pk, GTUNSLICE, "GTUNSLICE");
      SetDomainSettings(IniData, Sec, pk, ECORE, "ECORE");

      // On hybrid CPUs IACORE/RING/ECORE rails are often shared or coupled;
      // large divergence between their offsets can cause instability.
      if (!gAppQuietMode && gActiveCpuData && gActiveCpuData->HasEcores && gCpuInfo.HybridArch) {
        const INT16 offCore = pk->planes[IACORE].OffsetVolts;
        const INT16 offRing = pk->planes[RING].OffsetVolts;
        const INT16 offE    = pk->planes[ECORE].OffsetVolts;
        const INT16 warnDelta = 30; // mV

        if ((AbsI16(offCore - offRing) > warnDelta) ||
            (AbsI16(offCore - offE) > warnDelta) ||
            (AbsI16(offRing - offE) > warnDelta)) {
          UiAsciiPrint("WARNING: IACORE/RING/ECORE offsets differ by >%d mV.\n", warnDelta);
          UiAsciiPrint("         On hybrid CPUs these rails are often coupled; keep them close.\n");
        }
      }

      // Power Limits MSR
      pk->ProgramPL12_MSR = (UINT8)IniGetInt(IniData, Sec, "ProgramPL12_MSR", 0);
      pk->EnableMsrPkgPL1 = (UINT8)IniGetInt(IniData, Sec, "EnableMsrPkgPL1", pk->ProgramPL12_MSR);
      pk->EnableMsrPkgPL2 = (UINT8)IniGetInt(IniData, Sec, "EnableMsrPkgPL2", pk->ProgramPL12_MSR);
      pk->MsrPkgPL1_Power = IniGetInt(IniData, Sec, "MsrPkgPL1_Power", MAX_POWAH);
      pk->MsrPkgPL2_Power = IniGetInt(IniData, Sec, "MsrPkgPL2_Power", MAX_POWAH);
      pk->MsrPkgPL_Time = IniGetInt(IniData, Sec, "MsrPkgPL_Time", MAX_POWAH);
      pk->ClampMsrPkgPL = (UINT8)IniGetInt(IniData, Sec, "ClampMsrPkgPL", 0);
      pk->LockMsrPkgPL12 = (UINT8)IniGetInt(IniData, Sec, "LockMsrPkgPL12", 0);

      // MMIO
      pk->ProgramPL12_MMIO = (UINT8)IniGetInt(IniData, Sec, "ProgramPL12_MMIO", 0);
      pk->EnableMmioPkgPL1 = (UINT8)IniGetInt(IniData, Sec, "EnableMmioPkgPL1", pk->ProgramPL12_MMIO);
      pk->EnableMmioPkgPL2 = (UINT8)IniGetInt(IniData, Sec, "EnableMmioPkgPL2", pk->ProgramPL12_MMIO);
      pk->MmioPkgPL1_Power = IniGetInt(IniData, Sec, "MmioPkgPL1_Power", MAX_POWAH);
      pk->MmioPkgPL2_Power = IniGetInt(IniData, Sec, "MmioPkgPL2_Power", MAX_POWAH);
      pk->MmioPkgPL_Time = IniGetInt(IniData, Sec, "MmioPkgPL_Time", MAX_POWAH);
      pk->ClampMmioPkgPL = (UINT8)IniGetInt(IniData, Sec, "ClampMmioPkgPL", 0);
      pk->LockMmioPkgPL12 = (UINT8)IniGetInt(IniData, Sec, "LockMmioPkgPL12", 0);

      // Platform
      pk->ProgramPL12_PSys = (UINT8)IniGetInt(IniData, Sec, "ProgramPL12_PSys", 0);
      pk->EnablePlatformPL1 = (UINT8)IniGetInt(IniData, Sec, "EnablePlatformPL1", pk->ProgramPL12_PSys);
      pk->EnablePlatformPL2 = (UINT8)IniGetInt(IniData, Sec, "EnablePlatformPL2", pk->ProgramPL12_PSys);
      pk->PlatformPL1_Power = IniGetInt(IniData, Sec, "PlatformPL1_Power", MAX_POWAH);
      pk->PlatformPL2_Power = IniGetInt(IniData, Sec, "PlatformPL2_Power", MAX_POWAH);
      pk->PlatformPL_Time = IniGetInt(IniData, Sec, "PlatformPL_Time", MAX_POWAH);
      pk->ClampPlatformPL = (UINT8)IniGetInt(IniData, Sec, "ClampPlatformPL", 0);
      pk->LockPlatformPL = (UINT8)IniGetInt(IniData, Sec, "LockPlatformPL", 0);

      // PL3, PL4, PP0
      pk->ProgramPL3 = (UINT8)IniGetInt(IniData, Sec, "ProgramPL3", 0);
      pk->EnableMsrPkgPL3 = (UINT8)IniGetInt(IniData, Sec, "EnableMsrPkgPL3", pk->ProgramPL3);
      pk->MsrPkgPL3_Power = IniGetInt(IniData, Sec, "MsrPkgPL3_Power", MAX_POWAH);
      pk->MsrPkgPL3_Time = IniGetInt(IniData, Sec, "MsrPkgPL3_Time", MAX_POWAH);
      pk->LockMsrPkgPL3 = (UINT8)IniGetInt(IniData, Sec, "LockMsrPkgPL3", 0);

      pk->ProgramPL4 = (UINT8)IniGetInt(IniData, Sec, "ProgramPL4", 0);
      pk->EnableMsrPkgPL4 = (UINT8)IniGetInt(IniData, Sec, "EnableMsrPkgPL4", pk->ProgramPL4);
      pk->MsrPkgPL4_Current = IniGetInt(IniData, Sec, "MsrPkgPL4_Current", MAX_POWAH);
      pk->LockMsrPkgPL4 = (UINT8)IniGetInt(IniData, Sec, "LockMsrPkgPL4", 0);

      pk->ProgramPP0 = (UINT8)IniGetInt(IniData, Sec, "ProgramPP0", 0);
      pk->EnableMsrPkgPP0 = (UINT8)IniGetInt(IniData, Sec, "EnableMsrPkgPP0", pk->ProgramPP0);
      pk->MsrPkgPP0_Power = IniGetInt(IniData, Sec, "MsrPkgPP0_Power", MAX_POWAH);
      pk->MsrPkgPP0_Time = IniGetInt(IniData, Sec, "MsrPkgPP0_Time", MAX_POWAH);
      pk->ClampMsrPP0 = (UINT8)IniGetInt(IniData, Sec, "ClampMsrPP0", 0);
      pk->LockMsrPP0 = (UINT8)IniGetInt(IniData, Sec, "LockMsrPP0", 0);
    }
  }

  if (IniData) {
    gBS->FreePool(IniData);
  }
}
