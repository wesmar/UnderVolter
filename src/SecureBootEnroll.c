// SecureBootEnroll.c — Reads PK.auth / KEK.auth / db.auth / dbx.auth from the
//                      ESP and enrolls them into the UEFI Secure Boot variable
//                      store via gRT->SetVariable with time-based authentication.
//
// Controlled by the [SecureBoot] section in UnderVolter.ini:
//
//   [SecureBoot]
//   SecureBootEnroll  = 1          ; master switch for .auth file enrollment (default 0)
//   KeyDir            = \EFI\keys  ; directory containing .auth files on ESP
//   EnrollDBX         = 0          ; enroll dbx.auth  (default 0)
//   EnrollDB          = 1          ; enroll db.auth   (default 1)
//   EnrollKEK         = 1          ; enroll KEK.auth  (default 1)
//   EnrollPK          = 1          ; enroll PK.auth   (default 1)
//   RebootAfterEnroll = 1          ; warm reset after PK enrolled (default 1)
//
//   SelfEnroll              = 1     ; auto-enroll embedded root CA cert → Secure Boot
//   SelfEnrollReboot        = 1    ; warm reset after enrollment (default 1)
//   BootToFirmwareUI        = 0    ; ask to boot to BIOS/FW UI after reboot (default 0)
//   BootToFirmwareUITimeout = 5    ; seconds to wait for Y/N answer (default 5)
//   TryDeployedMode         = 1    ; after enroll try DeployedMode=1 via UEFI variable (default 1)
//                                  ; set to 0 if your firmware ignores it and the prompt annoys you
//
// SelfEnroll workflow (run once, requires BIOS in Setup Mode):
//   1. BIOS → delete/clear PK → enter Setup Mode → reboot
//   2. Set SelfEnroll = 1, run UnderVolter.efi from UEFI shell
//   3. Root CA cert is written to db, KEK, PK; firmware reboots
//   4. Secure Boot is now active; any UnderVolter.efi signed with the leaf
//      signing cert (.\Signer\sign.ps1) is trusted by the firmware
//   5. Set SelfEnroll = 0 (enrollment is persistent; re-run would be a no-op anyway)
//
// Certificate setup (run once on the developer machine):
//   .\Signer\sign.ps1 -Create     generates root CA + signing cert pair
//   .\build.ps1                    embeds cert into EFI, signs the output
//
// .auth file enrollment:
//   Enrollment order: dbx → db → KEK → PK (UEFI spec order).
//   PK written last: enrolling it transitions Setup Mode → User Mode.
//   In Setup Mode (PK not yet set) firmware accepts unauthenticated writes.
//   In User Mode all .auth files must be signed with the current KEK/PK.

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include "SecureBootEnroll.h"
#include "Config.h"
#include "UiConsole.h"
#include "UnderVolterCert.h"

// ─── GUIDs ───────────────────────────────────────────────────────────────────

// {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}  EFI global variable namespace
static EFI_GUID gSbeGlobalVarGuid = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C }
};

// {C1C41626-504C-4092-ACA9-41F936934328}  EFI_CERT_SHA256_GUID (hash db entries)
static EFI_GUID gSbeCertSha256Guid = {
    0xC1C41626, 0x504C, 0x4092,
    { 0xAC, 0xA9, 0x41, 0xF9, 0x36, 0x93, 0x43, 0x28 }
};

// {A5C059A1-94E4-4AA7-87B5-AB155C2BF072}  EFI_CERT_X509_GUID (X.509 cert in db/KEK/PK)
static EFI_GUID gSbeCertX509Guid = {
    0xA5C059A1, 0x94E4, 0x4AA7,
    { 0x87, 0xB5, 0xAB, 0x15, 0x5C, 0x2B, 0xF0, 0x72 }
};

/* // {4A6E5B1C-8F3D-4E92-A715-3C8DF21E6B04}  UnderVolter owner GUID
// Identifies UnderVolter's signature entries in db/KEK/PK; visible in KeyTool.
static EFI_GUID gSbeOwnerGuid = {
    0x4A6E5B1C, 0x8F3D, 0x4E92,
    { 0xA7, 0x15, 0x3C, 0x8D, 0xF2, 0x1E, 0x6B, 0x04 }
}; */

// Standard Microsoft GUID for third-party UEFI signatures in db
static EFI_GUID gSbeOwnerGuid = {
    0x77fa9abd, 0x0359, 0x4d32,
    { 0xbd, 0x60, 0x28, 0xf4, 0xe7, 0x8f, 0x78, 0x4b }
};

// {D719B2CB-3D3A-4596-A3BC-DAD00E67656F}  Image Security Database (db/dbx)
static EFI_GUID gSbeImageSecurityDb = {
    0xD719B2CB, 0x3D3A, 0x4596,
    { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F }
};

// {4AAFD29D-68DF-49EE-8AA9-347D375665A7}  EFI_CERT_TYPE_PKCS7_GUID (AUTH2 wrapper)
static EFI_GUID gSbePkcs7Guid = {
    0x4aafd29d, 0x68df, 0x49ee,
    { 0x8a, 0xa9, 0x34, 0x7d, 0x37, 0x56, 0x65, 0xa7 }
};

// {964E5B22-6459-11D2-8E39-00A0C969723B}  SimpleFileSystem
static EFI_GUID gSbeSfsProtocol = {
    0x964E5B22, 0x6459, 0x11D2,
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

// {09576E92-6D3F-11D2-8E39-00A0C969723B}  EFI_FILE_INFO
static EFI_GUID gSbeFileInfoGuid = {
    0x09576E92, 0x6D3F, 0x11D2,
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

// {5B1B31A1-9562-11D2-8E3F-00A0C969723B}  EFI_LOADED_IMAGE_PROTOCOL
static EFI_GUID gSbeLoadedImageGuid = {
    0x5B1B31A1, 0x9562, 0x11D2,
    { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

// ─── Variable attributes ─────────────────────────────────────────────────────

// Standard UEFI Secure Boot variable attributes (used for .auth file writes).
#define SBE_ATTR_BASE \
    (EFI_VARIABLE_NON_VOLATILE            | \
     EFI_VARIABLE_RUNTIME_ACCESS          | \
     EFI_VARIABLE_BOOTSERVICE_ACCESS      | \
     EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)

#define SBE_ATTR_APPEND  (SBE_ATTR_BASE | EFI_VARIABLE_APPEND_WRITE)


// ─── Config ──────────────────────────────────────────────────────────────────

#define SBE_MAX_PATH      256
#define SBE_MAX_FILE_SIZE (256 * 1024)

typedef struct {
    BOOLEAN  Enabled;
    CHAR16   KeyDir[SBE_MAX_PATH];
    BOOLEAN  EnrollDBX;
    BOOLEAN  EnrollDB;
    BOOLEAN  EnrollKEK;
    BOOLEAN  EnrollPK;
    BOOLEAN  RebootAfter;
    BOOLEAN  SelfEnroll;              // enroll embedded root CA cert into db/KEK/PK
    BOOLEAN  SelfEnrollReboot;        // warm reset after confirmed enrollment
    BOOLEAN  BootToFirmwareUI;        // ask to boot to BIOS/FW UI after reboot
    UINT32   BootToFirmwareUITimeout; // seconds to wait for Y/N (default 5)
    BOOLEAN  TryDeployedMode;         // attempt DeployedMode=1 via UEFI var after enroll
} SBE_CONFIG;

// ─── EFI_SIGNATURE_LIST layout ───────────────────────────────────────────────

// One contiguous block per signature type (SHA-256, X.509, etc.) in db/KEK/PK.
#pragma pack(1)
typedef struct {
    EFI_GUID  SignatureType;       // e.g. EFI_CERT_X509_GUID
    UINT32    SignatureListSize;   // total bytes including header + entries
    UINT32    SignatureHeaderSize; // extra bytes before first entry (usually 0)
    UINT32    SignatureSize;       // bytes per EFI_SIGNATURE_DATA entry
} SBE_SIG_LIST;
#pragma pack()

// ─── EFI_VARIABLE_AUTHENTICATION_2 header ─────────────────────────────────────
//
// SelfEnroll uses EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS even in
// Setup Mode.  This is required for maximum firmware compatibility: some OEM
// implementations (e.g. Dell) reject SetVariable() on Secure Boot variables
// when the flag is absent, even when the platform is in Setup Mode.
//
// In Setup Mode the firmware does NOT verify the signature — it skips the
// PKCS#7 check and processes the EFI_SIGNATURE_LIST payload directly.  We
// therefore provide an empty signature (no CertData bytes) with TimeStamp=0.
//
// Layout (40 bytes total):
//   [0..15]  EFI_TIME         TimeStamp  — all zeros (no time constraint)
//   [16..39] WIN_CERTIFICATE_UEFI_GUID   — dwLength=24, wRevision=0x0200,
//                                          wCertificateType=0x0EF1,
//                                          CertType=EFI_CERT_TYPE_PKCS7_GUID
//            <no CertData>
//   [40..]   EFI_SIGNATURE_LIST payload  (follows immediately after header)
#pragma pack(1)
typedef struct {
    EFI_TIME   TimeStamp;          // 16 bytes, all zeros
    UINT32     dwLength;           // = sizeof(SBE_AUTH2_HEADER) - sizeof(EFI_TIME) = 24
    UINT16     wRevision;          // 0x0200
    UINT16     wCertificateType;   // 0x0EF1  WIN_CERT_TYPE_EFI_GUID
    EFI_GUID   CertType;           // gSbePkcs7Guid — no CertData bytes follow
} SBE_AUTH2_HEADER;
#pragma pack()

// ─── Minimal INI helpers (no libc) ───────────────────────────────────────────

static BOOLEAN SbeMatchCI(CONST CHAR8* s, CONST CHAR8* lit) {
    while (*lit) {
        CHAR8 a = *s, b = *lit;
        if (a >= 'a' && a <= 'z') a = (CHAR8)(a - 32);
        if (b >= 'a' && b <= 'z') b = (CHAR8)(b - 32);
        if (a != b) return FALSE;
        s++; lit++;
    }
    return TRUE;
}

static UINTN SbeStrLen8(CONST CHAR8* s) {
    UINTN n = 0; while (*s++) n++; return n;
}

static BOOLEAN SbeReadBool(CONST CHAR8* p, BOOLEAN Default) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '=') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '1') return TRUE;
        if (*p == '0') return FALSE;
    }
    return Default;
}

static UINT32 SbeReadUint(CONST CHAR8* p, UINT32 Default) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return Default;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return Default;
    UINT32 v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (UINT32)(*p++ - '0');
    return v;
}

static VOID SbeReadPath(CONST CHAR8* p, CHAR16* Out, UINTN OutLen) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    UINTN i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < OutLen - 1) {
        CHAR8 c = *p++;
        Out[i++] = (c == '/') ? L'\\' : (CHAR16)c;
    }
    while (i > 0 && Out[i - 1] == L' ') i--;
    Out[i] = L'\0';
}

// ─── [SecureBoot] INI parser ─────────────────────────────────────────────────

static VOID ParseSecureBootSection(CONST CHAR8* IniData, SBE_CONFIG* Cfg) {
    Cfg->Enabled          = FALSE;
    Cfg->EnrollDBX        = FALSE;
    Cfg->EnrollDB         = TRUE;
    Cfg->EnrollKEK        = TRUE;
    Cfg->EnrollPK         = TRUE;
    Cfg->RebootAfter      = TRUE;
    Cfg->SelfEnroll              = FALSE;
    Cfg->SelfEnrollReboot        = TRUE;
    Cfg->BootToFirmwareUI        = FALSE;
    Cfg->BootToFirmwareUITimeout = 5;
    Cfg->TryDeployedMode         = TRUE;

    CONST CHAR16 kDefault[] = L"\\EFI\\keys";
    CopyMem(Cfg->KeyDir, kDefault, sizeof(kDefault));

    if (!IniData) return;

    CONST CHAR8* p      = IniData;
    BOOLEAN      inSect = FALSE;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if (*p == ';' || *p == '#') { while (*p && *p != '\n') p++; continue; }

        if (*p == '[') {
            p++;
            inSect = SbeMatchCI(p, "SecureBoot") && (p[10] == ']');
            while (*p && *p != '\n') p++;
            continue;
        }

        if (inSect) {
            // Note: longer keys must be checked before their prefixes (SelfEnrollReboot before SelfEnroll).
            if      (SbeMatchCI(p, "SecureBootEnroll"))  { Cfg->Enabled          = SbeReadBool(p + SbeStrLen8("SecureBootEnroll"),  FALSE); }
            else if (SbeMatchCI(p, "KeyDir"))             { SbeReadPath(p + SbeStrLen8("KeyDir"), Cfg->KeyDir, SBE_MAX_PATH); }
            else if (SbeMatchCI(p, "EnrollDBX"))          { Cfg->EnrollDBX        = SbeReadBool(p + SbeStrLen8("EnrollDBX"),         FALSE); }
            else if (SbeMatchCI(p, "EnrollDB"))           { Cfg->EnrollDB         = SbeReadBool(p + SbeStrLen8("EnrollDB"),          TRUE);  }
            else if (SbeMatchCI(p, "EnrollKEK"))          { Cfg->EnrollKEK        = SbeReadBool(p + SbeStrLen8("EnrollKEK"),         TRUE);  }
            else if (SbeMatchCI(p, "EnrollPK"))           { Cfg->EnrollPK         = SbeReadBool(p + SbeStrLen8("EnrollPK"),          TRUE);  }
            else if (SbeMatchCI(p, "RebootAfterEnroll"))  { Cfg->RebootAfter      = SbeReadBool(p + SbeStrLen8("RebootAfterEnroll"), TRUE);  }
            else if (SbeMatchCI(p, "SelfEnrollReboot"))        { Cfg->SelfEnrollReboot        = SbeReadBool(p + SbeStrLen8("SelfEnrollReboot"),        TRUE);  }
            else if (SbeMatchCI(p, "SelfEnroll"))              { Cfg->SelfEnroll               = SbeReadBool(p + SbeStrLen8("SelfEnroll"),               FALSE); }
            else if (SbeMatchCI(p, "BootToFirmwareUITimeout")) { Cfg->BootToFirmwareUITimeout  = SbeReadUint(p + SbeStrLen8("BootToFirmwareUITimeout"),  5);     }
            else if (SbeMatchCI(p, "BootToFirmwareUI"))        { Cfg->BootToFirmwareUI         = SbeReadBool(p + SbeStrLen8("BootToFirmwareUI"),         FALSE); }
            else if (SbeMatchCI(p, "TryDeployedMode"))         { Cfg->TryDeployedMode          = SbeReadBool(p + SbeStrLen8("TryDeployedMode"),          TRUE);  }
        }

        while (*p && *p != '\n') p++;
    }
}

// ─── File helpers ─────────────────────────────────────────────────────────────

static CHAR16* SbeBuildPath(CONST CHAR16* Dir, CONST CHAR16* FileName) {
    UINTN DirLen  = 0; while (Dir[DirLen])      DirLen++;
    UINTN FileLen = 0; while (FileName[FileLen]) FileLen++;
    BOOLEAN NeedSlash = (DirLen > 0 && Dir[DirLen-1] != L'\\' && Dir[DirLen-1] != L'/');
    UINTN Total = DirLen + (NeedSlash ? 1 : 0) + FileLen;
    CHAR16* Out = AllocateZeroPool((Total + 1) * sizeof(CHAR16));
    if (!Out) return NULL;
    UINTN Pos = 0;
    for (UINTN i = 0; i < DirLen;  i++) Out[Pos++] = Dir[i];
    if (NeedSlash)                       Out[Pos++] = L'\\';
    for (UINTN i = 0; i < FileLen; i++) Out[Pos++] = FileName[i];
    Out[Total] = L'\0';
    return Out;
}

static EFI_STATUS SbeOpenFile(
    IN  EFI_BOOT_SERVICES* BS,
    IN  EFI_HANDLE         DeviceHandle,
    IN  CONST CHAR16*      FilePath,
    OUT EFI_FILE_PROTOCOL** OutFile
) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
    EFI_STATUS Status = BS->HandleProtocol(DeviceHandle, &gSbeSfsProtocol, (VOID**)&Fs);
    if (EFI_ERROR(Status) || !Fs) return EFI_NOT_FOUND;
    EFI_FILE_PROTOCOL* Root = NULL;
    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status) || !Root) return Status;
    Status = Root->Open(Root, OutFile, (CHAR16*)FilePath, EFI_FILE_MODE_READ, 0);
    Root->Close(Root);
    return Status;
}

static EFI_STATUS SbeReadFile(
    IN  EFI_FILE_PROTOCOL* File,
    OUT VOID**             OutBuf,
    OUT UINTN*             OutSize
) {
    UINTN InfoSize = SIZE_OF_EFI_FILE_INFO + 512;
    EFI_FILE_INFO* Info = AllocatePool(InfoSize);
    if (!Info) return EFI_OUT_OF_RESOURCES;
    EFI_STATUS Status = File->GetInfo(File, &gSbeFileInfoGuid, &InfoSize, Info);
    if (EFI_ERROR(Status)) { FreePool(Info); return Status; }
    UINTN FileSize = (UINTN)Info->FileSize;
    FreePool(Info);
    if (FileSize == 0)                return EFI_NOT_FOUND;
    if (FileSize > SBE_MAX_FILE_SIZE) return EFI_BAD_BUFFER_SIZE;
    VOID* Buf = AllocatePool(FileSize);
    if (!Buf) return EFI_OUT_OF_RESOURCES;
    UINTN ReadSize = FileSize;
    Status = File->Read(File, &ReadSize, Buf);
    if (EFI_ERROR(Status) || ReadSize != FileSize) {
        FreePool(Buf);
        return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
    }
    *OutBuf  = Buf;
    *OutSize = FileSize;
    return EFI_SUCCESS;
}

// ─── Single .auth variable enroll ────────────────────────────────────────────

static VOID EnrollOneVar(
    IN EFI_BOOT_SERVICES*    BS,
    IN EFI_RUNTIME_SERVICES* RT,
    IN EFI_HANDLE            DeviceHandle,
    IN CONST CHAR16*         KeyDir,
    IN CONST CHAR16*         FileName,
    IN CONST CHAR16*         VarName,
    IN EFI_GUID*             VarGuid,
    IN UINT32                Attrs,
    IN BOOLEAN               QuietMode
) {
    CHAR16* FullPath = SbeBuildPath(KeyDir, FileName);
    if (!FullPath) {
        if (!QuietMode) UiPrint(L"[SBE]   %s: AllocatePool failed\n", VarName);
        return;
    }

    EFI_FILE_PROTOCOL* File = NULL;
    EFI_STATUS Status = SbeOpenFile(BS, DeviceHandle, FullPath, &File);
    FreePool(FullPath);

    if (EFI_ERROR(Status)) {
        if (!QuietMode) UiPrint(L"[SBE]   %-6s  not found (0x%x) — skipped\n", VarName, Status);
        return;
    }

    VOID*  Buf  = NULL;
    UINTN  Size = 0;
    Status = SbeReadFile(File, &Buf, &Size);
    File->Close(File);

    if (EFI_ERROR(Status)) {
        if (!QuietMode) UiPrint(L"[SBE]   %-6s  read error (0x%x) — skipped\n", VarName, Status);
        return;
    }

    if (!QuietMode) UiPrint(L"[SBE]   %-6s  %u bytes ... ", VarName, (UINT32)Size);
    Status = RT->SetVariable((CHAR16*)VarName, VarGuid, Attrs, Size, Buf);
    FreePool(Buf);

    if (!QuietMode) {
        if (EFI_ERROR(Status)) UiPrint(L"FAILED (0x%x)\n", Status);
        else                   UiPrint(L"OK\n");
    }
}

// ─── SelfEnroll helpers ───────────────────────────────────────────────────────

static BOOLEAN SbeGuidEqual(CONST EFI_GUID* A, CONST EFI_GUID* B) {
    CONST UINT8* a = (CONST UINT8*)A;
    CONST UINT8* b = (CONST UINT8*)B;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++)
        if (a[i] != b[i]) return FALSE;
    return TRUE;
}

// Return TRUE if gUVCertDer is already present in VarName (db or KEK or PK).
// Walks EFI_SIGNATURE_LIST chain; matches EFI_CERT_X509 entries by DER bytes.
static BOOLEAN SbeCertInVar(
    IN EFI_RUNTIME_SERVICES* RT,
    IN CONST CHAR16*         VarName,
    IN EFI_GUID*             VarGuid
) {
#pragma warning(suppress: 6326)   // UV_CERT_DER_SIZE is a compile-time constant (by design)
    if (UV_CERT_DER_SIZE == 0) return FALSE;

    UINTN  Size  = 0;
    UINT32 Attrs = 0;
    EFI_STATUS s = RT->GetVariable((CHAR16*)VarName, VarGuid, &Attrs, &Size, NULL);
    if (s != EFI_BUFFER_TOO_SMALL || Size == 0) return FALSE;

    UINT8* Buf = AllocatePool(Size);
    if (!Buf) return FALSE;

    s = RT->GetVariable((CHAR16*)VarName, VarGuid, &Attrs, &Size, Buf);
    if (EFI_ERROR(s)) { FreePool(Buf); return FALSE; }

    BOOLEAN Found = FALSE;
    UINT8*  p     = Buf;
    UINT8*  End   = Buf + Size;

    while (!Found && p + sizeof(SBE_SIG_LIST) <= End) {
        SBE_SIG_LIST* List = (SBE_SIG_LIST*)p;

        if (List->SignatureListSize < sizeof(SBE_SIG_LIST) ||
            p + List->SignatureListSize > End) break;

        // X.509 lists have exactly one entry; SignatureSize = sizeof(EFI_GUID) + cert_size.
        UINTN ExpSigSize = sizeof(EFI_GUID) + UV_CERT_DER_SIZE;
        if (SbeGuidEqual(&List->SignatureType, &gSbeCertX509Guid) &&
            List->SignatureSize == (UINT32)ExpSigSize) {

            UINT8* Entry   = p + sizeof(SBE_SIG_LIST) + List->SignatureHeaderSize;
            UINT8* ListEnd = p + List->SignatureListSize;

            while (Entry + List->SignatureSize <= ListEnd) {
                UINT8* CertData = Entry + sizeof(EFI_GUID);  // skip owner GUID
                if (CompareMem(CertData, gUVCertDer, UV_CERT_DER_SIZE) == 0) {
                    Found = TRUE; break;
                }
                Entry += List->SignatureSize;
            }
        }
        p += List->SignatureListSize;
    }

    FreePool(Buf);
    return Found;
}

// Write gUVCertDer as a single EFI_SIGNATURE_LIST (EFI_CERT_X509_GUID) to VarName.
// Returns TRUE only when the cert is confirmed present after the write (fail-closed).
// Caller passes SBE_ATTR_APPEND for db/KEK (preserves existing entries) and
// SBE_ATTR_BASE for PK (replaces; transitions Setup Mode → User Mode).
//
// Data layout written to SetVariable:
//   [SBE_AUTH2_HEADER][SBE_SIG_LIST][EFI_GUID owner][DER cert bytes]
//
// The AUTH2 header is required because the variables carry
// EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS in their Attributes.
// In Setup Mode the firmware skips signature verification but still parses
// dwLength to locate the EFI_SIGNATURE_LIST payload.  After a successful write
// the firmware stores only the payload; GetVariable() returns the stripped list.
static BOOLEAN SbeWriteCertToVar(
    IN EFI_RUNTIME_SERVICES* RT,
    IN CONST CHAR16*         VarName,
    IN EFI_GUID*             VarGuid,
    IN UINT32                Attrs
) {
    UINTN SigListSize = sizeof(SBE_SIG_LIST) + sizeof(EFI_GUID) + UV_CERT_DER_SIZE;
    UINTN TotalSize   = sizeof(SBE_AUTH2_HEADER) + SigListSize;
    UINT8* Buf = AllocateZeroPool(TotalSize);
    if (!Buf) return FALSE;

    // AUTH2 header — TimeStamp stays zero (AllocateZeroPool); no time constraint.
    // In Setup Mode the firmware does not verify the empty PKCS#7 signature.
    SBE_AUTH2_HEADER* Auth = (SBE_AUTH2_HEADER*)Buf;
    Auth->dwLength          = (UINT32)(sizeof(SBE_AUTH2_HEADER) - sizeof(EFI_TIME));
    Auth->wRevision         = 0x0200;
    Auth->wCertificateType  = 0x0EF1;  // WIN_CERT_TYPE_EFI_GUID
    Auth->CertType          = gSbePkcs7Guid;

    SBE_SIG_LIST* List     = (SBE_SIG_LIST*)(Buf + sizeof(SBE_AUTH2_HEADER));
    List->SignatureType       = gSbeCertX509Guid;
    List->SignatureListSize   = (UINT32)SigListSize;
    List->SignatureHeaderSize = 0;
    List->SignatureSize       = (UINT32)(sizeof(EFI_GUID) + UV_CERT_DER_SIZE);

    UINT8* Entry = (UINT8*)List + sizeof(SBE_SIG_LIST);
    CopyMem(Entry,                    &gSbeOwnerGuid, sizeof(EFI_GUID));
    CopyMem(Entry + sizeof(EFI_GUID), gUVCertDer,     UV_CERT_DER_SIZE);

    EFI_STATUS s = RT->SetVariable((CHAR16*)VarName, VarGuid, Attrs, TotalSize, Buf);
    FreePool(Buf);

    if (EFI_ERROR(s)) return FALSE;

    // Verify: confirm cert is present in the variable after the write (fail-closed).
    // Firmware strips the AUTH2 header on storage; GetVariable returns raw SigList.
    // PK is not readable after User Mode is entered — skip verify for PK.
    if (Attrs & EFI_VARIABLE_APPEND_WRITE) {
        return SbeCertInVar(RT, VarName, VarGuid);
    }
    return TRUE;  // PK write: trust non-error return; Setup Mode just ended
}

// ─── SetupMode check ─────────────────────────────────────────────────────────

static BOOLEAN SbeIsSetupMode(IN EFI_RUNTIME_SERVICES* RT) {
    UINT8  Mode = 0;
    UINTN  Size = sizeof(Mode);
    EFI_STATUS s = RT->GetVariable(L"SetupMode", &gSbeGlobalVarGuid, NULL, &Size, &Mode);
    return (!EFI_ERROR(s) && Mode == 1);
}

// ─── Reboot helpers ──────────────────────────────────────────────────────────

// Attempt the standard UEFI 2.5+ Audit Mode → Deployed Mode transition by
// writing DeployedMode=1 to the EFI global variable namespace.
//
// Per UEFI spec this write is allowed from Audit Mode without authentication
// (you are moving to a MORE restrictive state).  The reverse (Deployed→Audit)
// requires an AUTH2-signed PK deletion — that's what failed in earlier tests.
//
// Returns TRUE on EFI_SUCCESS.  Non-fatal: if the firmware rejects it (Dell
// may have its own internal state machine), the caller falls through to the
// BootToFirmwareUI prompt or a manual BIOS UI visit.
static EFI_STATUS SbeTryDeployedMode(IN EFI_RUNTIME_SERVICES* RT) {
    UINT8 one = 1;
    return RT->SetVariable(
        L"DeployedMode",
        &gSbeGlobalVarGuid,
        EFI_VARIABLE_NON_VOLATILE |
        EFI_VARIABLE_BOOTSERVICE_ACCESS |
        EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(one), &one);
}

// Set EFI_OS_INDICATIONS_BOOT_TO_FW_UI so the next warm reset enters the
// firmware/BIOS setup UI (the same bit Windows sets on Shift+Restart → UEFI).
static VOID SbeSetBootToFwUI(IN EFI_RUNTIME_SERVICES* RT) {
    UINT64 OsInd = 0;
    UINTN  Size  = sizeof(OsInd);
    UINT32 Attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    // Read-modify-write — preserve any other bits that may already be set.
    RT->GetVariable(L"OsIndications", &gSbeGlobalVarGuid, &Attrs, &Size, &OsInd);
    OsInd |= 0x0000000000000001ULL;  // EFI_OS_INDICATIONS_BOOT_TO_FW_UI
    RT->SetVariable(L"OsIndications", &gSbeGlobalVarGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(OsInd), &OsInd);
}

// Ask the user whether to boot to BIOS/FW UI.
// Shows "[Y/N] (Ns)" countdown.  Returns TRUE only on explicit Y keypress.
// Any other key or timeout → returns FALSE (normal reboot).
static BOOLEAN SbeAskBootToFwUI(
    IN EFI_BOOT_SERVICES* BS,
    IN EFI_SYSTEM_TABLE*  ST,
    IN UINT32             TimeoutSec
) {
    if (TimeoutSec == 0) TimeoutSec = 1;
    for (UINT32 Sec = TimeoutSec; Sec > 0; Sec--) {
        UiPrint(L"\r[SBE] Boot to BIOS/firmware UI? [Y/N] (%us)  ", Sec);
        for (UINTN J = 0; J < 100; J++) {
            EFI_STATUS Ev = BS->CheckEvent(ST->ConIn->WaitForKey);
            if (!EFI_ERROR(Ev)) {
                EFI_INPUT_KEY Key = { 0, 0 };
                ST->ConIn->ReadKeyStroke(ST->ConIn, &Key);
                CHAR16 UC = Key.UnicodeChar;
                if (UC >= L'a' && UC <= L'z') UC = (CHAR16)(UC - L'a' + L'A');
                UiPrint(L"\n");
                return (UC == L'Y');
            }
            BS->Stall(10000);  // 10 ms
        }
    }
    UiPrint(L"\n");
    return FALSE;  // timeout → normal reboot
}

// Shared reboot sequence for both enrollment paths.
//   BootToFirmwareUI = 0: shows 3 s countdown with any-key cancel.
//   BootToFirmwareUI = 1: asks [Y/N]; Y → sets OsIndications before reset.
// Returns TRUE if the user cancelled (no reboot took place).
static BOOLEAN SbeDoReboot(
    IN EFI_BOOT_SERVICES*    BS,
    IN EFI_RUNTIME_SERVICES* RT,
    IN EFI_SYSTEM_TABLE*     ST,
    IN CONST SBE_CONFIG*     Cfg
) {
    if (!gAppQuietMode) {
        if (Cfg->BootToFirmwareUI) {
            if (SbeAskBootToFwUI(BS, ST, Cfg->BootToFirmwareUITimeout)) {
                UiPrint(L"[SBE] Rebooting to BIOS/firmware UI...\n");
                SbeSetBootToFwUI(RT);
            } else {
                UiPrint(L"[SBE] Rebooting normally...\n");
            }
        } else {
            for (UINT32 Sec = 3; Sec > 0; Sec--) {
                UiPrint(L"\r[SBE] Rebooting in %u s... (any key to cancel)  ", Sec);
                for (UINTN J = 0; J < 100; J++) {
                    EFI_STATUS Ev = BS->CheckEvent(ST->ConIn->WaitForKey);
                    if (!EFI_ERROR(Ev)) {
                        EFI_INPUT_KEY Key = { 0, 0 };
                        ST->ConIn->ReadKeyStroke(ST->ConIn, &Key);
                        UiPrint(L"\n[SBE] Reboot cancelled.\n");
                        return TRUE;
                    }
                    BS->Stall(10000);
                }
            }
            UiPrint(L"\n");
        }
    }
    RT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
    return FALSE;
}

// ─── Public API ──────────────────────────────────────────────────────────────

VOID EnrollSecureBootKeys(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
) {
    SBE_CONFIG Cfg;
    ParseSecureBootSection(GetIniDataPtr(), &Cfg);

    EFI_BOOT_SERVICES*    BS = SystemTable->BootServices;
    EFI_RUNTIME_SERVICES* RT = SystemTable->RuntimeServices;

    // ── SelfEnroll: enroll embedded root CA cert into db, KEK, PK ────────────
    //
    // Idempotent: if our cert is already in db, skip silently — no reboot.
    // Fail-closed: reboot only after all three writes are confirmed.
    //
    // SetupMode is shown as diagnostics but NOT used as a gate.  Reason:
    // some OEM firmware (e.g. Dell) reports SetupMode=0 even in their own
    // "Audit Mode" / "Custom Mode", yet still accepts unauthenticated writes
    // when those modes are active.  We let SetVariable() decide — if the
    // firmware rejects the write it returns an error and we do not reboot.
    if (Cfg.SelfEnroll) {
#pragma warning(suppress: 6326)   // UV_CERT_DER_SIZE is a compile-time constant (by design)
        if (UV_CERT_DER_SIZE == 0) {
            if (!gAppQuietMode)
                UiPrint(L"[SBE] SelfEnroll: no certificate embedded.\n"
                        L"[SBE]   Run: .\\Signer\\sign.ps1 -Create  then  .\\build.ps1\n");

        } else if (SbeCertInVar(RT, L"db", &gSbeImageSecurityDb)) {
            // Already enrolled (cert found in db) — no action, no reboot.
            if (!gAppQuietMode)
                UiPrint(L"[SBE] SelfEnroll: certificate already enrolled — no action needed.\n");

        } else {
            if (!gAppQuietMode) {
                BOOLEAN IsStdSetupMode = SbeIsSetupMode(RT);
                if (IsStdSetupMode) {
                    UiPrint(L"[SBE] SelfEnroll: Setup Mode confirmed. Enrolling root CA certificate...\n");
                } else {
                    UiPrint(L"[SBE] SelfEnroll: SetupMode=0 (Dell Audit/Custom Mode?). Attempting enrollment...\n");
                }
            }

            // Write order: db → KEK → PK.
            // db/KEK: APPEND_WRITE preserves existing Microsoft CA entries.
            // PK: REPLACE (only one PK allowed); exits Setup Mode after write.
            BOOLEAN dbOk  = SbeWriteCertToVar(RT, L"db",  &gSbeImageSecurityDb, SBE_ATTR_APPEND);
            BOOLEAN kekOk = SbeWriteCertToVar(RT, L"KEK", &gSbeGlobalVarGuid,   SBE_ATTR_APPEND);
            BOOLEAN pkOk  = SbeWriteCertToVar(RT, L"PK",  &gSbeGlobalVarGuid,   SBE_ATTR_BASE);

            if (!gAppQuietMode) {
                UiPrint(L"[SBE]   db:  %s\n", dbOk  ? L"OK" : L"FAILED");
                UiPrint(L"[SBE]   KEK: %s\n", kekOk ? L"OK" : L"FAILED");
                // PK write fails when a Microsoft PK is already present (normal after
                // "Reset All Keys" which restores defaults, not empties them).
                // db enrollment alone is sufficient for EFI binary trust; PK controls
                // who may update KEK/db in the future, not which binaries are trusted.
                if (pkOk) {
                    UiPrint(L"[SBE]   PK:  OK\n");
                } else {
                    UiPrint(L"[SBE]   PK:  not replaced (Microsoft default PK active)\n");
                }
            }

            // Reboot requires db and KEK enrolled — the cert is in the trust database.
            // A PK failure is non-critical: Microsoft PK remaining means future db/KEK
            // updates need Microsoft-signed auth, but UnderVolter.efi is already trusted.
            if (!dbOk || !kekOk) {
                if (!gAppQuietMode)
                    UiPrint(L"[SBE] SelfEnroll: db/KEK write failed — NOT rebooting.\n"
                            L"[SBE]   Ensure BIOS is in Audit Mode + Custom Mode.\n");
            } else {
                // Try Audit→Deployed transition via standard UEFI variable.
                // Do this BEFORE printing the summary so the status is visible.
                EFI_STATUS deployedSt = EFI_NOT_STARTED;
                if (Cfg.TryDeployedMode)
                    deployedSt = SbeTryDeployedMode(RT);

                if (!gAppQuietMode) {
                    if (pkOk) {
                        UiPrint(L"[SBE] SelfEnroll: all keys enrolled. Secure Boot active after reboot.\n");
                    } else {
                        UiPrint(L"[SBE] SelfEnroll: db and KEK enrolled. Microsoft PK retained.\n");
                    }
                    UiPrint(L"[SBE]   Future UnderVolter.efi signed with the leaf cert will be trusted.\n");

                    if (Cfg.TryDeployedMode) {
                        if (!EFI_ERROR(deployedSt)) {
                            UiPrint(L"[SBE]   DeployedMode: OK — Secure Boot will enforce after reboot.\n");
                        } else {
                            UiPrint(L"[SBE]   DeployedMode: firmware rejected (0x%lx) — use BootToFirmwareUI=1\n"
                                    L"[SBE]   or switch to Deployed Mode manually in BIOS after reboot.\n",
                                    (UINTN)deployedSt);
                        }
                    } else if (!pkOk) {
                        UiPrint(L"[SBE]   After reboot switch BIOS to Deployed Mode to activate Secure Boot.\n");
                    }
                }

                if (!Cfg.SelfEnrollReboot) {
                    if (!gAppQuietMode)
                        UiPrint(L"[SBE] SelfEnrollReboot = 0 — reboot manually to activate Secure Boot.\n");
                } else {
                    SbeDoReboot(BS, RT, SystemTable, &Cfg);
                }
            }
        }
    }

    if (!Cfg.Enabled) return;

    // ── .auth file enrollment ─────────────────────────────────────────────────

    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
    EFI_STATUS Status = BS->HandleProtocol(
        ImageHandle, &gSbeLoadedImageGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status) || !LoadedImage) {
        if (!gAppQuietMode)
            UiPrint(L"[SBE] Cannot resolve LoadedImage protocol (0x%x)\n", Status);
        return;
    }
    EFI_HANDLE DeviceHandle = LoadedImage->DeviceHandle;

    BOOLEAN InSetupMode = SbeIsSetupMode(RT);

    if (!gAppQuietMode) {
        UiPrint(L"[SBE] Secure Boot .auth key enrollment\n");
        UiPrint(L"[SBE] Mode: %s\n",
                InSetupMode ? L"Setup Mode (unauthenticated writes allowed)"
                            : L"User Mode  (auth required — .auth files must be signed)");
        UiPrint(L"[SBE] KeyDir: %s\n", Cfg.KeyDir);
    }

    // Enroll in spec order: dbx → db → KEK → PK
    if (Cfg.EnrollDBX)
        EnrollOneVar(BS, RT, DeviceHandle, Cfg.KeyDir,
            L"dbx.auth", L"dbx", &gSbeImageSecurityDb,
            SBE_ATTR_APPEND, gAppQuietMode);

    if (Cfg.EnrollDB)
        EnrollOneVar(BS, RT, DeviceHandle, Cfg.KeyDir,
            L"db.auth", L"db", &gSbeImageSecurityDb,
            SBE_ATTR_APPEND, gAppQuietMode);

    if (Cfg.EnrollKEK)
        EnrollOneVar(BS, RT, DeviceHandle, Cfg.KeyDir,
            L"KEK.auth", L"KEK", &gSbeGlobalVarGuid,
            SBE_ATTR_APPEND, gAppQuietMode);

    if (Cfg.EnrollPK)
        EnrollOneVar(BS, RT, DeviceHandle, Cfg.KeyDir,
            L"PK.auth", L"PK", &gSbeGlobalVarGuid,
            SBE_ATTR_BASE, gAppQuietMode);  // no APPEND for PK — replaces

    if (!gAppQuietMode)
        UiPrint(L"[SBE] Enrollment complete.\n");

    if (!Cfg.RebootAfter) {
        if (!gAppQuietMode)
            UiPrint(L"[SBE] RebootAfterEnroll = 0 — reboot manually to activate Secure Boot.\n");
        return;
    }

    SbeDoReboot(BS, RT, SystemTable, &Cfg);
}
