// Firmware regression tests. All configuration writes use an in-memory runtime.
#include <PiPei.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Protocol/MpService.h>
#include "../src/Config.c"
#include "../src/SecureBootEnroll.c"
#include "../src/NvramSetup.c"
#include "../src/InterruptHook.h"
#include "../src/HwIntrinsicsX64.h"
#include "../src/MpDispatcher.h"
#include "../src/OcMailbox.h"

static UINT64 MockRead(UINT32 Address);
static UINT32 MockWrite(UINT32 Address, UINT64 Value);
static VOID MockStall(UINT64 Us);
#define pm_rdmsr64 MockRead
#define pm_wrmsr64 MockWrite
#define MicroStall MockStall
#include "../src/CpuMailboxes.c"
#undef pm_rdmsr64
#undef pm_wrmsr64
#undef MicroStall

EFI_MP_SERVICES_PROTOCOL* gMpServices;
PLATFORM* gPlatform;
UINTN gBootCpu;
BOOLEAN gCpuDetected;
CONST UINT32 _gUefiDriverRevision = 0;
CHAR8* gEfiCallerBaseName = "UnderVolterRegression";

static UINTN Tests, Failures, Writes, Resets;
#define CHECK(expr) do { Tests++; if (!(expr)) { Failures++; Print(L"FAIL line %u: %a\r\n", (UINT32)__LINE__, #expr); } } while (0)
static EFI_GUID TestGuid;
typedef struct { CHAR16 Name[32]; UINT8 Data[16384]; UINTN Size; } TEST_VAR;
static TEST_VAR Vars[8];
static CONST CHAR16* RejectName;
static BOOLEAN IgnoreWrites;
static EFI_RUNTIME_SERVICES Runtime;
static EFI_BOOT_SERVICES Boot;
static EFI_SYSTEM_TABLE System;
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL RootFile, DataFile;
static UINT8* FileBytes;
static UINTN FileSize;
static BOOLEAN MissingFile, ShortRead;

static TEST_VAR* FindVar(CHAR16* Name) {
  for (UINTN i = 0; i < 8; i++) if (StrCmp(Vars[i].Name, Name) == 0) return &Vars[i];
  return NULL;
}
static EFI_STATUS EFIAPI GetVar(CHAR16* Name, EFI_GUID* Guid, UINT32* Attrs, UINTN* Size, VOID* Data) {
  (VOID)Guid;
  TEST_VAR* V = FindVar(Name);
  if (!V || !V->Size) return EFI_NOT_FOUND;
  if (Attrs) *Attrs = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  if (*Size < V->Size) { *Size = V->Size; return EFI_BUFFER_TOO_SMALL; }
  *Size = V->Size;
  CopyMem(Data, V->Data, V->Size);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetVar(CHAR16* Name, EFI_GUID* Guid, UINT32 Attrs, UINTN Size, VOID* Data) {
  (VOID)Guid;
  Writes++;
  if (RejectName && !StrCmp(Name, RejectName)) return EFI_WRITE_PROTECTED;
  if (IgnoreWrites) return EFI_SUCCESS;
  TEST_VAR* V = FindVar(Name);
  if (!V) {
    for (UINTN i = 0; i < 8; i++) if (!Vars[i].Name[0]) { V = &Vars[i]; StrCpyS(V->Name, 32, Name); break; }
  }
  if (!V) return EFI_OUT_OF_RESOURCES;
  if (Attrs & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) {
    SBE_AUTH2_HEADER* Auth = Data;
    UINTN Offset = sizeof(EFI_TIME) + Auth->dwLength;
    if (Offset > Size) return EFI_INVALID_PARAMETER;
    Data = (UINT8*)Data + Offset;
    Size -= Offset;
  }
  UINTN Base = (Attrs & EFI_VARIABLE_APPEND_WRITE) ? V->Size : 0;
  if (Base + Size > sizeof(V->Data)) return EFI_OUT_OF_RESOURCES;
  CopyMem(V->Data + Base, Data, Size);
  V->Size = Base + Size;
  return EFI_SUCCESS;
}
static VOID EFIAPI Reset(EFI_RESET_TYPE Type, EFI_STATUS Status, UINTN Size, VOID* Data) {
  (VOID)Type; (VOID)Status; (VOID)Size; (VOID)Data;
  Resets++;
}
static EFI_STATUS EFIAPI Time(EFI_TIME* T, EFI_TIME_CAPABILITIES* Cap) {
  (VOID)Cap;
  ZeroMem(T, sizeof(*T)); T->Year = 2026; T->Month = 9; T->Day = 6;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL* F) { (VOID)F; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL* F, EFI_FILE_PROTOCOL** Out, CHAR16* Name, UINT64 Mode, UINT64 Attrs) {
  (VOID)F; (VOID)Name; (VOID)Mode; (VOID)Attrs;
  if (MissingFile) return EFI_NOT_FOUND;
  *Out = &DataFile; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* S, EFI_FILE_PROTOCOL** F) {
  (VOID)S; *F = &RootFile; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL* F, EFI_GUID* Guid, UINTN* Size, VOID* Buf) {
  (VOID)F; (VOID)Guid;
  UINTN Needed = SIZE_OF_EFI_FILE_INFO + 32;
  if (*Size < Needed) { *Size = Needed; return EFI_BUFFER_TOO_SMALL; }
  *Size = Needed; ZeroMem(Buf, Needed);
  ((EFI_FILE_INFO*)Buf)->Size = Needed;
  ((EFI_FILE_INFO*)Buf)->FileSize = FileSize;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL* F, UINTN* Size, VOID* Buf) {
  (VOID)F;
  *Size = MIN(*Size, FileSize);
  if (ShortRead && *Size) (*Size)--;
  CopyMem(Buf, FileBytes, *Size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Protocol(EFI_HANDLE Handle, EFI_GUID* Guid, VOID** Out) {
  (VOID)Handle;
  if (!CompareMem(Guid, &gEfiLoadedImageProtocolGuid, sizeof(*Guid))) *Out = &Loaded;
  else if (!CompareMem(Guid, &gEfiSimpleFileSystemProtocolGuid, sizeof(*Guid))) *Out = &Fs;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID* Guid, VOID* Key, UINTN* Count, EFI_HANDLE** Handles) {
  (VOID)Type; (VOID)Guid; (VOID)Key;
  *Count = 0; *Handles = NULL; return EFI_NOT_FOUND;
}
static VOID InitMock(VOID) {
  ZeroMem(Vars, sizeof(Vars)); RejectName = NULL; IgnoreWrites = FALSE;
  Writes = Resets = 0; MissingFile = ShortRead = FALSE;
  Runtime.GetVariable = GetVar; Runtime.SetVariable = SetVar;
  Runtime.ResetSystem = Reset; Runtime.GetTime = Time;
  Boot = *gBS; Boot.HandleProtocol = Protocol; Boot.LocateHandleBuffer = Locate;
  System = *gST; System.RuntimeServices = &Runtime; System.BootServices = &Boot;
  Fs.OpenVolume = Volume;
  RootFile.Open = Open; RootFile.Close = Close;
  DataFile.Read = Read; DataFile.GetInfo = Info; DataFile.Close = Close;
  Loaded.DeviceHandle = (EFI_HANDLE)1; Loaded.FilePath = NULL;
  gAppQuietMode = TRUE;
}
static VOID UseIni(CONST CHAR8* Text) {
  if (gIniData) FreePool(gIniData);
  gIniData = AllocateCopyPool(AsciiStrSize(Text), Text);
  gIniLoaded = TRUE; gIniFound = TRUE;
}

static VOID TestParsers(VOID) {
  INT64 Number;
  CHECK(TryParseInt("-90 ; offset", &Number) && Number == -90);
  CHECK(!TryParseInt("9223372036854775808", &Number));
  CHECK(!TryParseInt("45garbage", &Number));
  CHECK(IniGetInt("[Global]\nX=-1\n", "Global", "X", 7) == 7);
  CHECK(IniGetInt("[ global ]\nX=4294967295\n", "Global", "X", 7) == MAX_UINT32);
  CHECK(!IniReadBool("=10", FALSE));
  CHECK(IniReadUint("=4294967296", 7) == 7);
  CHAR16 Path[80] = {0};
  IniReadPath("= \"/EFI/keys\" ; comment", Path, 80);
  CHECK(!StrCmp(Path, L"\\EFI\\keys"));
  CHAR8 Section[64];
  CHECK(FindProfileSection("[Profile.CFL]\nArchitecture = \"CoffeeLake\" ; comment\n", "CoffeeLake", Section));
  CHECK(!AsciiStrCmp(Section, "Profile.CFL"));
  CHECK(!ParseGuid("A", &TestGuid));
  PACKAGE* P = AllocateZeroPool(sizeof(*P));
  P->planes[IACORE].OffsetVolts = -75;
  P->planes[IACORE].nVfPoints = 1;
  P->planes[IACORE].vfPoint[0].FusedRatio = 40;
  P->planes[IACORE].vfPoint[0].IsValid = 1;
  gBCLK_bsp = 100000;
  SetDomainSettings("[P]\n", "P", P, IACORE, "IACORE");
  CHECK(P->Program_VF_Overrides[IACORE] == 0 && P->planes[IACORE].OffsetVolts == -75);
  SetDomainSettings("[P]\nOffsetVolts_IACORE=0\n", "P", P, IACORE, "IACORE");
  CHECK(P->Program_VF_Overrides[IACORE] == 1 && P->planes[IACORE].OffsetVolts == 0);
  SetDomainSettings("[P]\nVF_Point_0_IACORE=4000\nOther=1:-100\n", "P", P, IACORE, "IACORE");
  CHECK(P->planes[IACORE].vfPoint[0].VOffset == 0);
  FreePool(P);
  EFI_BOOT_SERVICES* RealBoot = gBS;
  gBS = &Boot;
  CHAR8 Bom[] = "\xef\xbb\xbf[Global]\nQuietMode=1\n";
  FileBytes = (UINT8*)Bom; FileSize = sizeof(Bom)-1;
  CHAR8* Data = NULL; UINTN Size = 0;
  CHECK(ReadIniFile(&Data, &Size) == EFI_SUCCESS && Size == sizeof(Bom)-4 && Data[0] == '[');
  if (Data) FreePool(Data);
  ShortRead = TRUE;
  CHECK(ReadIniFile(&Data, &Size) == EFI_DEVICE_ERROR);
  ShortRead = FALSE; MissingFile = TRUE;
  CHECK(ReadIniFile(&Data, &Size) == EFI_NOT_FOUND);
  MissingFile = FALSE; FileSize = 1024 * 1024 + 1;
  CHECK(ReadIniFile(&Data, &Size) == EFI_BAD_BUFFER_SIZE);
  gBS = RealBoot;
}

static VOID TestCertificates(VOID) {
  InitMock();
  UseIni("[SecureBoot]\nSelfEnroll=1\nSelfEnrollReboot=1\nTryDeployedMode=0\n");
  // Existing platform key is retained; db and KEK enroll once.
  UINT8 ExistingPk = 1;
  SetVar(L"PK", &TestGuid, 0, 1, &ExistingPk); Writes = 0;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 2 && Resets == 1);
  CHECK(SbeCertInVar(&Runtime, L"db", &gSbeImageSecurityDb));
  CHECK(SbeCertInVar(&Runtime, L"KEK", &gSbeGlobalVarGuid));
  Writes = Resets = 0;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 0 && Resets == 0);
  // Partial enrollment must not write PK or reboot; next pass repairs KEK.
  InitMock(); RejectName = L"KEK";
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 2 && Resets == 0 && !FindVar(L"PK"));
  RejectName = NULL; Writes = 0;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 2 && Resets == 1 && FindVar(L"PK"));
  InitMock(); IgnoreWrites = TRUE;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 1 && Resets == 0);
  // .auth missing, malformed, rejected, unchanged and verified writes.
  InitMock();
  UseIni("[SecureBoot]\nSecureBootEnroll=1\nRebootAfterEnroll=1\nEnrollKEK=0\nEnrollPK=0\n");
  MissingFile = TRUE;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 0 && Resets == 0);
  MissingFile = FALSE;
  UINT8 Invalid[64] = {0}; FileBytes = Invalid; FileSize = sizeof(Invalid);
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 0 && Resets == 0);
  CHECK(SbeWriteCertToVar(&Runtime, L"db", &gSbeImageSecurityDb, SBE_ATTR_APPEND));
  TEST_VAR* Db = FindVar(L"db");
  UINTN AuthSize = sizeof(SBE_AUTH2_HEADER) + Db->Size;
  UINT8* Auth = AllocateZeroPool(AuthSize);
  SBE_AUTH2_HEADER* H = (SBE_AUTH2_HEADER*)Auth;
  H->dwLength = sizeof(*H) - sizeof(EFI_TIME); H->wRevision = 0x200;
  H->wCertificateType = 0xef1; H->CertType = gSbePkcs7Guid;
  CopyMem(Auth + sizeof(*H), Db->Data, Db->Size);
  FileBytes = Auth; FileSize = AuthSize; Writes = Resets = 0;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 0 && Resets == 0);
  Db->Size = 0; RejectName = L"db";
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 1 && Resets == 0);
  RejectName = NULL; Writes = 0;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 1 && Resets == 1);
  Writes = Resets = 0;
  EnrollSecureBootKeys(gImageHandle, &System);
  CHECK(Writes == 0 && Resets == 0);
  ((SBE_SIG_LIST*)(Auth + sizeof(*H)))->SignatureHeaderSize = MAX_UINT32;
  CHECK(!SbeValidLists(Auth + sizeof(*H), AuthSize - sizeof(*H)));
  FreePool(Auth);
}

static VOID TestNvram(VOID) {
  InitMock();
  UINT8 Setup[16] = { 1, 1 };
  SetVar(L"Setup", &TestGuid, 0, sizeof(Setup), Setup); Writes = 0;
  UseIni("[SetupVar]\nNvramPatchEnabled=1\nPatch_0=0:0\nNvramPatchReboot=1\n");
  ApplyNvramSetupPatches(&System);
  CHECK(Writes == 1 && Resets == 1 && FindVar(L"Setup")->Data[0] == 0);
  Writes = Resets = 0; ApplyNvramSetupPatches(&System);
  CHECK(!Writes && !Resets);
  UseIni("[SetupVar]\nNvramPatchEnabled=1\nPatch_0=1:0\nPatch_1=100:0\n");
  ApplyNvramSetupPatches(&System); CHECK(!Writes && !Resets);
  UseIni("[SetupVar]\nNvramPatchEnabled=1\nPatch_0=1:0\nPatch_1=1:1\n");
  ApplyNvramSetupPatches(&System); CHECK(!Writes && !Resets);
  UseIni("[SetupVar]\nNvramPatchEnabled=1\nPatch_0=1:0\n");
  IgnoreWrites = TRUE;
  ApplyNvramSetupPatches(&System); CHECK(Writes == 1 && !Resets);
}

static UINT64 Sequence[32], Requests[8];
static UINTN ReadIndex, RequestCount;
static UINT64 MockRead(UINT32 Address) { (VOID)Address; return Sequence[ReadIndex++ % 32]; }
static UINT32 MockWrite(UINT32 Address, UINT64 Value) { (VOID)Address; Requests[RequestCount++ % 8] = Value; return 0; }
static VOID MockStall(UINT64 Us) { (VOID)Us; }
static VOID TestMailbox(VOID) {
  CpuMailbox Box;
  ZeroMem(Sequence, sizeof(Sequence)); ReadIndex = RequestCount = 0;
  OcMailbox_InitializeAsMSR(&Box);
  // Only data changes between readbacks. Retry must use the original request.
  Sequence[1] = 1; Sequence[2] = 2; Sequence[4] = Sequence[5] = 3;
  CHECK(OcMailbox_ReadWrite(0x123410, 0x456, &Box) == EFI_SUCCESS);
  CHECK(RequestCount == 2 && Requests[0] == Requests[1]);
  ZeroMem(Sequence, sizeof(Sequence)); ReadIndex = RequestCount = 0;
  Sequence[1] = Sequence[2] = 1ULL << 32;
  CHECK(EFI_ERROR(OcMailbox_ReadWrite(0x10, 0, &Box)) && Box.status == 1);
}

EFI_STATUS DetectPackages(PLATFORM* P);
static EFI_PROCESSOR_INFORMATION Processors[6];
static EFI_STATUS EFIAPI ProcessorInfo(EFI_MP_SERVICES_PROTOCOL* Mp, UINTN Index, EFI_PROCESSOR_INFORMATION* Out) {
  (VOID)Mp;
  if (Index >= 6) return EFI_NOT_FOUND;
  *Out = Processors[Index]; return EFI_SUCCESS;
}
static VOID TestTopology(VOID) {
  EFI_MP_SERVICES_PROTOCOL Mp = {0};
  Mp.GetProcessorInfo = ProcessorInfo;
  gMpServices = &Mp;
  for (UINTN i = 0; i < 6; i++) {
    Processors[i].ProcessorId = i;
    Processors[i].Location.Package = (UINT32)(i % 2 + 10);
    Processors[i].StatusFlag = PROCESSOR_ENABLED_BIT;
  }
  Processors[2].StatusFlag = 0;
  PLATFORM* P = AllocateZeroPool(sizeof(*P));
  P->LogicalProcessors = 6;
  CHECK(DetectPackages(P) == EFI_SUCCESS && P->PkgCnt == 2);
  CHECK(P->packages[0].LogicalCores == 2 && P->packages[1].LogicalCores == 3);
  CHECK(gCorePtrs[2] == NULL && ((CPUCORE*)gCorePtrs[4])->PkgIdx == 0);
  ZeroMem(P, sizeof(*P)); P->LogicalProcessors = MAX_CORES * MAX_PACKAGES + 1;
  CHECK(DetectPackages(P) == EFI_UNSUPPORTED);
  FreePool(P); ZeroMem(gCorePtrs, sizeof(gCorePtrs)); gNumCores = 0; gMpServices = NULL;
}

static volatile UINT32 DispatchCount;
static VOID EFIAPI CountCpu(VOID* Arg) { (VOID)Arg; AtomicIncrementU32((UINT32*)&DispatchCount); }
static volatile UINT8* FindOpcode(VOID* Function, UINT8 SecondByte) {
  volatile UINT8* Code = Function;
  for (UINTN i = 0; i < 32; i++) if (Code[i] == 0x0f && Code[i+1] == SecondByte) return Code + i + 1;
  return NULL;
}
static VOID PatchOpcode(volatile UINT8* Byte, UINT8 Value) {
  // Test image only: inject UD2 at the exact guarded access site, then restore it.
  BOOLEAN Interrupts = SaveAndDisableInterrupts();
  UINTN Cr0 = AsmReadCr0();
  AsmWriteCr0(Cr0 & ~((UINTN)1 << 16));
  *Byte = Value;
  AsmWriteCr0(Cr0);
  SetInterruptState(Interrupts);
}
static VOID TestHardwareHelpers(VOID) {
  extern UINT64 gTscFreq;
  gTscFreq = 2000000000ULL;
  CHECK(TicksToNanoSeconds(2) == 1);
  CHECK(TicksToNanoSeconds(50000000001ULL) == 25000000000ULL);
  gTscFreq = 1;
  CHECK(TicksToNanoSeconds(MAX_UINT64) == MAX_UINT64);
  gTscFreq = 0;
  CHECK(TicksToNanoSeconds(1) == 0);
  InstallSafeAsmExceptionHandler();
  UINT64 gs = (UINT64)GetCpuGSBase();
  UINT32 error = 0;
  UINT64 value = 0;
  volatile UINT8* ReadOpcode = FindOpcode((VOID*)SafeReadMsr64, 0x32);
  volatile UINT8* WriteOpcode = FindOpcode((VOID*)SafeWriteMsr64, 0x30);
  CHECK(ReadOpcode != NULL && WriteOpcode != NULL);
  if (ReadOpcode && WriteOpcode) {
    PatchOpcode(ReadOpcode, 0x0b);
    value = SafeReadMsr64(0xdeadbeef, &error);
    PatchOpcode(ReadOpcode, 0x32);
    CHECK(error == 1 && value == 0);
    PatchOpcode(WriteOpcode, 0x0b);
    UINT32 WriteError = SafeWriteMsr64(0xdeadbeef, 0);
    PatchOpcode(WriteOpcode, 0x30);
    CHECK(WriteError == 1);
  }
  CHECK((UINT64)GetCpuGSBase() == gs);
  CHECK(SafeMmioWrite32(3, 0) == 1 && SafeMmioOr32(3, 0) == 1);
  value = SafeMmioRead32(3, &error);
  CHECK(error == 1 && value == 0);
  RemoveAllInterruptOverrides();
  if (EFI_ERROR(gBS->LocateProtocol(&gEfiMpServiceProtocolGuid, NULL, (VOID**)&gMpServices))) { CHECK(FALSE); return; }
  PLATFORM* P = AllocateZeroPool(sizeof(*P));
  gMpServices->WhoAmI(gMpServices, &P->BootProcessor);
  gMpServices->GetNumberOfProcessors(gMpServices, &P->LogicalProcessors, &P->EnabledLogicalProcessors);
  CHECK(DetectPackages(P) == EFI_SUCCESS);
  DispatchCount = 0;
  CHECK(RunOnAllProcessors(CountCpu, TRUE, NULL) == EFI_SUCCESS);
  CHECK(DispatchCount == P->EnabledLogicalProcessors);
  FreePool(P); ZeroMem(gCorePtrs, sizeof(gCorePtrs)); gNumCores = 0; gMpServices = NULL;
}

EFI_STATUS EFIAPI UefiUnload(IN EFI_HANDLE ImageHandle) { (VOID)ImageHandle; return EFI_SUCCESS; }

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* ST) {
  (VOID)ImageHandle; (VOID)ST;
  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  Print(L"UnderVolter regression tests\r\n");
  InitMock(); TestParsers(); TestCertificates(); TestNvram(); TestMailbox(); TestTopology(); TestHardwareHelpers();
  ReleaseAppSettings();
  Print(L"RESULT: %u checks, %u failures\r\n", (UINT32)Tests, (UINT32)Failures);
  EFI_LOADED_IMAGE_PROTOCOL* Self = NULL;
  if (!EFI_ERROR(gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&Self))) {
    EFI_DEVICE_PATH_PROTOCOL* Path = FileDevicePath(Self->DeviceHandle, L"\\application.efi");
    EFI_HANDLE Application = NULL;
    EFI_STATUS Status = gBS->LoadImage(FALSE, gImageHandle, Path, NULL, 0, &Application);
    FreePool(Path);
    if (!EFI_ERROR(Status)) {
      Status = gBS->StartImage(Application, NULL, NULL);
      Print(L"APPLICATION returned: %r\r\n", Status);
    }
  }
  gBS->Stall(1000000);
  gRT->ResetSystem(EfiResetShutdown, Failures ? EFI_ABORTED : EFI_SUCCESS, 0, NULL);
  return Failures ? EFI_ABORTED : EFI_SUCCESS;
}
