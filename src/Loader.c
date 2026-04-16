// Loader.c — Minimal EFI loader that finds UnderVolter.efi on any EFI System
//            Partition and chainloads it before handing off to the normal boot
//            sequence.  Search order: loader's own directory → preferred dirs
//            (e.g. \EFI\OC\Drivers) → recursive scan → all ESP handles.
//            After UnderVolter returns, chainloads the next entry in BootOrder
//            (respecting BootNext and BootCurrent), skipping itself and known
//            loader names to prevent infinite loops.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#define UNDERVOLTER_FILENAME   L"UnderVolter.efi"
#define BOOT_VAR_NAME_LEN      9
#define LOAD_OPTION_ACTIVE     0x00000001

STATIC CONST CHAR16* mPreferredDirs[] = {
  L"\\EFI\\OC\\Drivers"
};

extern CONST UINT32 _gUefiDriverRevision = 0;
CHAR8* gEfiCallerBaseName = "UnderVolterLoader";
EFI_STATUS EFIAPI UefiUnload(IN EFI_HANDLE ImageHandle) { return EFI_SUCCESS; }

// Case-fold a single CHAR16 in the ASCII range (a-z → A-Z).
STATIC
CHAR16
ToUpperAscii(
  IN CHAR16 c
  )
{
  if (c >= L'a' && c <= L'z') {
    return (CHAR16)(c - (L'a' - L'A'));
  }
  return c;
}

STATIC
INTN
StriCmp(
  IN CONST CHAR16* A,
  IN CONST CHAR16* B
  )
{
  if (A == NULL || B == NULL) {
    return (A == B) ? 0 : 1;
  }

  while (*A != L'\0' || *B != L'\0') {
    CHAR16 CA = ToUpperAscii(*A);
    CHAR16 CB = ToUpperAscii(*B);
    if (CA != CB) {
      return (CA < CB) ? -1 : 1;
    }
    if (*A == L'\0') {
      break;
    }
    A++;
    B++;
  }
  return 0;
}

// Case-insensitive path comparison: same as StriCmp but also treats '/' == '\\'.
STATIC
INTN
PathCmpInsensitive(
  IN CONST CHAR16* A,
  IN CONST CHAR16* B
  )
{
  if (A == NULL || B == NULL) {
    return (A == B) ? 0 : 1;
  }

  while (*A != L'\0' || *B != L'\0') {
    CHAR16 CA = ToUpperAscii(*A);
    CHAR16 CB = ToUpperAscii(*B);
    if (CA == L'/') {
      CA = L'\\';
    }
    if (CB == L'/') {
      CB = L'\\';
    }
    if (CA != CB) {
      return (CA < CB) ? -1 : 1;
    }
    if (*A == L'\0') {
      break;
    }
    A++;
    B++;
  }
  return 0;
}

STATIC
BOOLEAN
IsDotOrDotDot(
  IN CONST CHAR16* Name
  )
{
  if (Name == NULL) return FALSE;
  if (Name[0] != L'.') return FALSE;
  if (Name[1] == L'\0') return TRUE;
  return (Name[1] == L'.' && Name[2] == L'\0');
}

STATIC
CHAR16*
BuildPath(
  IN CONST CHAR16* Base,
  IN CONST CHAR16* Name
  )
{
  UINTN BaseLen = (Base != NULL) ? StrLen(Base) : 0;
  UINTN NameLen = (Name != NULL) ? StrLen(Name) : 0;
  if (NameLen == 0) return NULL;

  BOOLEAN NeedSlash = TRUE;
  if (BaseLen == 0) {
    NeedSlash = FALSE; // Root is empty or L"\\"
  } else if (Base[BaseLen - 1] == L'\\' || Base[BaseLen - 1] == L'/') {
    NeedSlash = FALSE;
  }

  UINTN TotalLen = BaseLen + (NeedSlash ? 1 : 0) + NameLen;
  CHAR16* Out = (CHAR16*)AllocateZeroPool((TotalLen + 1) * sizeof(CHAR16));
  if (Out == NULL) return NULL;

  UINTN Pos = 0;
  if (BaseLen > 0) {
    CopyMem(Out, Base, BaseLen * sizeof(CHAR16));
    Pos = BaseLen;
  }
  if (NeedSlash) {
    Out[Pos++] = L'\\';
  }
  CopyMem(&Out[Pos], Name, NameLen * sizeof(CHAR16));
  Out[TotalLen] = L'\0';
  return Out;
}

STATIC
CONST CHAR16*
GetFilePathFromDevicePath(
  IN EFI_DEVICE_PATH_PROTOCOL* Path
  )
{
  if (Path == NULL) {
    return NULL;
  }

  CONST CHAR16* LastPath = NULL;
  EFI_DEVICE_PATH_PROTOCOL* Node = Path;
  while (!IsDevicePathEnd(Node)) {
    if (DevicePathType(Node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType(Node) == MEDIA_FILEPATH_DP) {
      FILEPATH_DEVICE_PATH* FileNode = (FILEPATH_DEVICE_PATH*)Node;
      LastPath = FileNode->PathName;
    }
    Node = NextDevicePathNode(Node);
  }
  return LastPath;
}

STATIC
CHAR16*
ExtractDirPath(
  IN CONST CHAR16* FilePath
  )
{
  if (FilePath == NULL) {
    return NULL;
  }

  INTN LastSlash = -1;
  UINTN Len = StrLen(FilePath);
  for (UINTN i = 0; i < Len; i++) {
    if (FilePath[i] == L'\\' || FilePath[i] == L'/') {
      LastSlash = (INTN)i;
    }
  }

  if (LastSlash <= 0) {
    return AllocateCopyPool(sizeof(L"\\"), L"\\");
  }

  UINTN OutLen = (UINTN)LastSlash;
  CHAR16* Out = (CHAR16*)AllocateZeroPool((OutLen + 1) * sizeof(CHAR16));
  if (Out == NULL) {
    return NULL;
  }
  CopyMem(Out, FilePath, OutLen * sizeof(CHAR16));
  Out[OutLen] = L'\0';
  return Out;
}

// Recursively walk Dir looking for a file named TargetName.  Maximum depth is
// 4 to avoid infinite loops on circular filesystems.  Returns the full path
// in *FoundPath (caller must FreePool) on EFI_SUCCESS.
STATIC
EFI_STATUS
FindFileRecursive(
  IN EFI_FILE_PROTOCOL* Dir,
  IN CONST CHAR16* CurrentPath,
  IN CONST CHAR16* TargetName,
  IN UINTN Depth,
  OUT CHAR16** FoundPath
  )
{
  if (Dir == NULL || TargetName == NULL || FoundPath == NULL) return EFI_INVALID_PARAMETER;
  *FoundPath = NULL;

  if (Depth > 4) return EFI_NOT_FOUND;

  Dir->SetPosition(Dir, 0);

  UINTN InfoSize = SIZE_OF_EFI_FILE_INFO + 512;
  EFI_FILE_INFO* Info = (EFI_FILE_INFO*)AllocateZeroPool(InfoSize);
  if (Info == NULL) return EFI_OUT_OF_RESOURCES;

  EFI_STATUS Status = EFI_SUCCESS;
  while (TRUE) {
    UINTN ReadSize = InfoSize;
    Status = Dir->Read(Dir, &ReadSize, Info);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      FreePool(Info);
      InfoSize = ReadSize;
      Info = (EFI_FILE_INFO*)AllocateZeroPool(InfoSize);
      if (Info == NULL) return EFI_OUT_OF_RESOURCES;
      continue;
    }
    if (EFI_ERROR(Status) || ReadSize == 0) break;

    if (IsDotOrDotDot(Info->FileName)) continue;

    if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
      EFI_FILE_PROTOCOL* ChildDir = NULL;
      CHAR16* ChildPath = BuildPath(CurrentPath, Info->FileName);
      
      Status = Dir->Open(Dir, &ChildDir, Info->FileName, EFI_FILE_MODE_READ, 0);
      if (!EFI_ERROR(Status)) {
        Status = FindFileRecursive(ChildDir, ChildPath, TargetName, Depth + 1, FoundPath);
        ChildDir->Close(ChildDir);
        if (!EFI_ERROR(Status) && *FoundPath != NULL) {
          FreePool(ChildPath);
          break;
        }
      }
      if (ChildPath) FreePool(ChildPath);
    } else {
      if (StriCmp(Info->FileName, TargetName) == 0) {
        *FoundPath = BuildPath(CurrentPath, Info->FileName);
        Status = (*FoundPath != NULL) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
        break;
      }
    }
  }

  FreePool(Info);
  return Status;
}

STATIC
EFI_STATUS
FindUnderVolterOnDevice(
  IN EFI_HANDLE DeviceHandle,
  OUT CHAR16** FoundPath
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
  EFI_STATUS Status = gBS->HandleProtocol(DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
  if (EFI_ERROR(Status)) return Status;

  EFI_FILE_PROTOCOL* Root = NULL;
  Status = Fs->OpenVolume(Fs, &Root);
  if (EFI_ERROR(Status)) return Status;

  // Search starting from root
  Status = FindFileRecursive(Root, L"\\", UNDERVOLTER_FILENAME, 0, FoundPath);
  Root->Close(Root);
  return Status;
}

STATIC
EFI_STATUS
FindUnderVolterInPreferredDirs(
  IN EFI_HANDLE DeviceHandle,
  OUT CHAR16** FoundPath
  )
{
  if (FoundPath == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *FoundPath = NULL;

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
  EFI_STATUS Status = gBS->HandleProtocol(
    DeviceHandle,
    &gEfiSimpleFileSystemProtocolGuid,
    (VOID**)&Fs
  );
  if (EFI_ERROR(Status) || Fs == NULL) {
    return Status;
  }

  EFI_FILE_PROTOCOL* Root = NULL;
  Status = Fs->OpenVolume(Fs, &Root);
  if (EFI_ERROR(Status) || Root == NULL) {
    return Status;
  }

  for (UINTN i = 0; i < ARRAY_SIZE(mPreferredDirs); i++) {
    CHAR16* FullPath = BuildPath(mPreferredDirs[i], UNDERVOLTER_FILENAME);
    if (FullPath == NULL) {
      continue;
    }

    EFI_FILE_PROTOCOL* File = NULL;
    Status = Root->Open(Root, &File, FullPath, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(Status)) {
      *FoundPath = FullPath;
      File->Close(File);
      Root->Close(Root);
      return EFI_SUCCESS;
    }

    FreePool(FullPath);
  }

  Root->Close(Root);
  return EFI_NOT_FOUND;
}

// Heuristic: a volume is an ESP if it has an \EFI directory at its root.
STATIC
BOOLEAN
IsEfiSystemPartition(
  IN EFI_HANDLE DeviceHandle
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
  EFI_STATUS Status = gBS->HandleProtocol(DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
  if (EFI_ERROR(Status) || Fs == NULL) {
    return FALSE;
  }

  EFI_FILE_PROTOCOL* Root = NULL;
  Status = Fs->OpenVolume(Fs, &Root);
  if (EFI_ERROR(Status) || Root == NULL) {
    return FALSE;
  }

  EFI_FILE_PROTOCOL* EfiDir = NULL;
  Status = Root->Open(Root, &EfiDir, L"EFI", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR(Status) && EfiDir != NULL) {
    EfiDir->Close(EfiDir);
    Root->Close(Root);
    return TRUE;
  }

  Root->Close(Root);
  return FALSE;
}

STATIC
EFI_STATUS
FindUnderVolterInLoaderDir(
  IN EFI_HANDLE DeviceHandle,
  IN EFI_DEVICE_PATH_PROTOCOL* LoaderFilePath,
  OUT CHAR16** FoundPath
  )
{
  if (FoundPath == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *FoundPath = NULL;
  if (LoaderFilePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CONST CHAR16* LoaderPathStr = GetFilePathFromDevicePath(LoaderFilePath);
  if (LoaderPathStr == NULL) {
    return EFI_NOT_FOUND;
  }

  CHAR16* DirPath = ExtractDirPath(LoaderPathStr);
  if (DirPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  // Try to open the file in this directory
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
  EFI_STATUS Status = gBS->HandleProtocol(DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
  if (!EFI_ERROR(Status)) {
    EFI_FILE_PROTOCOL* Root = NULL;
    Status = Fs->OpenVolume(Fs, &Root);
    if (!EFI_ERROR(Status)) {
      EFI_FILE_PROTOCOL* File = NULL;
      CHAR16* FullPath = BuildPath(DirPath, UNDERVOLTER_FILENAME);
      Status = Root->Open(Root, &File, FullPath, EFI_FILE_MODE_READ, 0);
      if (!EFI_ERROR(Status)) {
        *FoundPath = FullPath;
        File->Close(File);
      } else {
        if (FullPath) FreePool(FullPath);
      }
      Root->Close(Root);
    }
  }

  FreePool(DirPath);
  return (*FoundPath != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
FindUnderVolterOnAllFileSystems(
  OUT CHAR16** FoundPath,
  OUT EFI_HANDLE* FoundDevice
  )
{
  UINTN HandleCount = 0;
  EFI_HANDLE* Handles = NULL;
  EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR(Status)) return Status;

  for (UINTN h = 0; h < HandleCount; h++) {
    if (!IsEfiSystemPartition(Handles[h])) continue;

    Status = FindUnderVolterInPreferredDirs(Handles[h], FoundPath);
    if (EFI_ERROR(Status) || *FoundPath == NULL) {
      Status = FindUnderVolterOnDevice(Handles[h], FoundPath);
    }
    if (!EFI_ERROR(Status) && *FoundPath != NULL) {
      *FoundDevice = Handles[h];
      break;
    }
  }

  if (Handles) FreePool(Handles);
  return (*FoundPath != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
BOOLEAN
IsSameDevicePath(
  IN EFI_DEVICE_PATH_PROTOCOL* A,
  IN EFI_DEVICE_PATH_PROTOCOL* B
  )
{
  if (A == NULL || B == NULL) return FALSE;
  UINTN SizeA = GetDevicePathSize(A);
  UINTN SizeB = GetDevicePathSize(B);
  if (SizeA != SizeB) return FALSE;
  return (CompareMem(A, B, SizeA) == 0);
}

STATIC
BOOLEAN
StrEndsWith(
  IN CONST CHAR16* Str,
  IN CONST CHAR16* Suffix
  )
{
  if (Str == NULL || Suffix == NULL) return FALSE;
  UINTN Len = StrLen(Str);
  UINTN SuffixLen = StrLen(Suffix);
  if (SuffixLen > Len) return FALSE;
  return PathCmpInsensitive(Str + (Len - SuffixLen), Suffix) == 0;
}

// Return TRUE if BootPath points back at this loader (Loader.efi, BOOTX64.EFI,
// or any path matching our own device+file path).  Prevents chainload loops.
STATIC
BOOLEAN
IsForbiddenTarget(
  IN EFI_DEVICE_PATH_PROTOCOL* SelfDevicePath,
  IN CONST CHAR16* SelfFilePath,
  IN EFI_DEVICE_PATH_PROTOCOL* BootPath
  )
{
  if (SelfDevicePath == NULL || BootPath == NULL) {
    return FALSE;
  }

  CONST CHAR16* BootFilePath = GetFilePathFromDevicePath(BootPath);
  if (BootFilePath == NULL) {
    return FALSE;
  }

  // 1. Exact match (as before)
  BOOLEAN IsMatch = (SelfFilePath != NULL && PathCmpInsensitive(SelfFilePath, BootFilePath) == 0);

  // 2. Known loader names on the same device
  if (!IsMatch) {
    IsMatch = StrEndsWith(BootFilePath, L"\\Loader.efi") || 
              StrEndsWith(BootFilePath, L"\\BOOTX64.EFI");
  }

  if (IsMatch) {
    UINTN SelfSize = GetDevicePathSize(SelfDevicePath);
    UINTN PrefixSize = (SelfSize > END_DEVICE_PATH_LENGTH) ? (SelfSize - END_DEVICE_PATH_LENGTH) : 0;
    if (CompareMem(BootPath, SelfDevicePath, PrefixSize) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

// Build the UEFI variable name for a boot option: "Boot" + 4 hex digits.
STATIC
VOID
BuildBootVarName(
  IN UINT16 BootNumber,
  OUT CHAR16* Name
  )
{
  STATIC CONST CHAR16 Hex[] = L"0123456789ABCDEF";
  Name[0] = L'B'; Name[1] = L'o'; Name[2] = L'o'; Name[3] = L't';
  Name[4] = Hex[(BootNumber >> 12) & 0xF];
  Name[5] = Hex[(BootNumber >> 8) & 0xF];
  Name[6] = Hex[(BootNumber >> 4) & 0xF];
  Name[7] = Hex[(BootNumber >> 0) & 0xF];
  Name[8] = L'\0';
}

// Read a BootNNNN UEFI variable and extract its device path and attributes.
// Parses the EFI_LOAD_OPTION structure: Attributes(4) + FilePathListLength(2)
// + Description(null-terminated CHAR16) + device path.  Returns
// EFI_COMPROMISED_DATA if the variable is malformed (unterminated description
// or FilePathListLength out of bounds).
STATIC
EFI_STATUS
LoadBootOptionDevicePath(
  IN UINT16 BootNumber,
  OUT EFI_DEVICE_PATH_PROTOCOL** OutPath,
  OUT UINT32* OutAttributes
  )
{
  CHAR16 VarName[BOOT_VAR_NAME_LEN];
  BuildBootVarName(BootNumber, VarName);

  UINTN DataSize = 0;
  EFI_STATUS Status = gRT->GetVariable(VarName, &gEfiGlobalVariableGuid, NULL, &DataSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) return Status;

  UINT8* Data = (UINT8*)AllocateZeroPool(DataSize);
  if (Data == NULL) return EFI_OUT_OF_RESOURCES;

  Status = gRT->GetVariable(VarName, &gEfiGlobalVariableGuid, NULL, &DataSize, Data);
  if (EFI_ERROR(Status)) {
    FreePool(Data);
    return Status;
  }

  // Ensure minimum size for Attributes and FilePathListLength
  if (DataSize < sizeof(UINT32) + sizeof(UINT16)) {
    FreePool(Data);
    return EFI_COMPROMISED_DATA;
  }

  UINT8* Ptr = Data;
  *OutAttributes = *(UINT32*)Ptr; Ptr += sizeof(UINT32);
  UINT16 FilePathListLength = *(UINT16*)Ptr; Ptr += sizeof(UINT16);

  // Safely find end of Description string within bounds
  CHAR16* Description = (CHAR16*)Ptr;
  UINTN MaxDescLen = (DataSize - (Ptr - Data)) / sizeof(CHAR16);
  UINTN DescLen = 0;
  while (DescLen < MaxDescLen && Description[DescLen] != L'\0') {
    DescLen++;
  }
  
  // If string isn't null-terminated within bounds, it's malformed
  if (DescLen >= MaxDescLen) {
    FreePool(Data);
    return EFI_COMPROMISED_DATA;
  }
  
  Ptr += (DescLen + 1) * sizeof(CHAR16);

  UINTN Remaining = (UINTN)(Data + DataSize - Ptr);

  // Bounds check for FilePathListLength
  if (FilePathListLength > 0 && FilePathListLength <= Remaining) {
    *OutPath = AllocateCopyPool(FilePathListLength, Ptr);
  } else if (FilePathListLength > Remaining) {
    FreePool(Data);
    return EFI_COMPROMISED_DATA;
  }

  FreePool(Data);
  return (*OutPath != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

// Load and start the boot option identified by BootNumber.  Skips inactive
// entries (LOAD_OPTION_ACTIVE not set) and forbidden targets (self-loops).
// Returns EFI_ALREADY_STARTED for forbidden targets so the caller can
// continue iterating without treating it as a hard error.
STATIC
EFI_STATUS
StartBootOptionByNumber(
  IN EFI_HANDLE ParentImage,
  IN UINT16 BootNumber,
  IN EFI_DEVICE_PATH_PROTOCOL* SelfDevicePath,
  IN CONST CHAR16* SelfFilePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL* BootPath = NULL;
  UINT32 Attributes = 0;
  EFI_STATUS Status = LoadBootOptionDevicePath(BootNumber, &BootPath, &Attributes);
  if (EFI_ERROR(Status)) return Status;

  if ((Attributes & LOAD_OPTION_ACTIVE) == 0) {
    FreePool(BootPath);
    return EFI_NOT_FOUND;
  }

  if (IsForbiddenTarget(SelfDevicePath, SelfFilePath, BootPath)) {
    FreePool(BootPath);
    return EFI_ALREADY_STARTED;
  }

  EFI_HANDLE ImageHandle = NULL;
  Status = gBS->LoadImage(FALSE, ParentImage, BootPath, NULL, 0, &ImageHandle);
  if (!EFI_ERROR(Status)) {
    Status = gBS->StartImage(ImageHandle, NULL, NULL);
  }

  FreePool(BootPath);
  return Status;
}

// Chainload the next boot target following the UEFI BootOrder / BootNext
// specification.  BootNext is consumed (deleted) and tried first.  Then
// iterates BootOrder starting at the entry after BootCurrent, skipping
// forbidden targets (EFI_ALREADY_STARTED).
STATIC
EFI_STATUS
StartDefaultBoot(
  IN EFI_HANDLE ParentImage,
  IN EFI_DEVICE_PATH_PROTOCOL* SelfDevicePath,
  IN CONST CHAR16* SelfFilePath
  )
{
  UINT16 BootCurrent = 0;
  UINTN BootCurrentSize = sizeof(BootCurrent);
  BOOLEAN HasBootCurrent = !EFI_ERROR(gRT->GetVariable(
    L"BootCurrent",
    &gEfiGlobalVariableGuid,
    NULL,
    &BootCurrentSize,
    &BootCurrent
  ));

  UINT16 BootNext = 0;
  UINTN BootNextSize = sizeof(BootNext);
  EFI_STATUS Status = gRT->GetVariable(L"BootNext", &gEfiGlobalVariableGuid, NULL, &BootNextSize, &BootNext);
  if (!EFI_ERROR(Status)) {
    // Consume BootNext as per UEFI spec (and suggestion)
    gRT->SetVariable(L"BootNext", &gEfiGlobalVariableGuid, 0, 0, NULL);

    Status = StartBootOptionByNumber(ParentImage, BootNext, SelfDevicePath, SelfFilePath);
    if (!EFI_ERROR(Status)) return Status;
  }

  UINTN BootOrderSize = 0;
  Status = gRT->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid, NULL, &BootOrderSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) return Status;

  UINT16* BootOrder = (UINT16*)AllocateZeroPool(BootOrderSize);
  Status = gRT->GetVariable(L"BootOrder", &gEfiGlobalVariableGuid, NULL, &BootOrderSize, BootOrder);
  if (EFI_ERROR(Status)) {
    if (BootOrder) FreePool(BootOrder);
    return Status;
  }

  UINTN Count = BootOrderSize / sizeof(UINT16);
  UINTN StartIndex = 0;

  if (HasBootCurrent) {
    for (UINTN i = 0; i < Count; i++) {
      if (BootOrder[i] == BootCurrent) {
        StartIndex = (i + 1) % Count;
        break;
      }
    }
  }

  EFI_STATUS LastStatus = EFI_NOT_FOUND;
  for (UINTN i = 0; i < Count; i++) {
    UINTN idx = (StartIndex + i) % Count;
    Status = StartBootOptionByNumber(ParentImage, BootOrder[idx], SelfDevicePath, SelfFilePath);
    if (!EFI_ERROR(Status)) {
      LastStatus = Status;
      break;
    }
    if (Status != EFI_ALREADY_STARTED) LastStatus = Status;
  }

  FreePool(BootOrder);
  return LastStatus;
}

// Locate UnderVolter.efi using the four-step search strategy described in the
// file header, build a full device path, and load+start it.
STATIC
EFI_STATUS
StartUnderVolter(
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS Status;
  CHAR16* FoundPath = NULL;
  EFI_HANDLE FoundDevice = NULL;

  EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
  Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
  if (!EFI_ERROR(Status)) {
    FoundDevice = LoadedImage->DeviceHandle;
    // 1. Try same directory as Loader
    Status = FindUnderVolterInLoaderDir(FoundDevice, LoadedImage->FilePath, &FoundPath);
    // 2. Try preferred locations on the same device
    if (EFI_ERROR(Status)) {
      Status = FindUnderVolterInPreferredDirs(FoundDevice, &FoundPath);
    }
    // 3. Try recursive search on the same device
    if (EFI_ERROR(Status)) {
      Status = FindUnderVolterOnDevice(FoundDevice, &FoundPath);
    }
  }

  // 4. Search all EFI file systems
  if (EFI_ERROR(Status) || FoundPath == NULL) {
    Status = FindUnderVolterOnAllFileSystems(&FoundPath, &FoundDevice);
  }

  if (EFI_ERROR(Status) || FoundPath == NULL) return Status;

  EFI_DEVICE_PATH* DevicePath = FileDevicePath(FoundDevice, FoundPath);
  FreePool(FoundPath);
  if (DevicePath == NULL) return EFI_OUT_OF_RESOURCES;

  EFI_HANDLE UnderVolterHandle = NULL;
  Status = gBS->LoadImage(FALSE, ImageHandle, DevicePath, NULL, 0, &UnderVolterHandle);
  FreePool(DevicePath);

  if (!EFI_ERROR(Status)) {
    Status = gBS->StartImage(UnderVolterHandle, NULL, NULL);
  }
  return Status;
}

// EFI application entry point.  Disables the watchdog, runs StartUnderVolter,
// then chainloads via StartDefaultBoot.  If nothing can be booted, exits with
// the last status code (firmware will try the next BootOrder entry on its own).
EFI_STATUS
EFIAPI
UefiMain(
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE* SystemTable
  )
{
  gBS->SetWatchdogTimer(0, 0, 0, NULL);  // disable firmware reboot-on-timeout

  // Try to start UnderVolter
  StartUnderVolter(ImageHandle);

  // Get device path + file path to avoid loops in StartDefaultBoot
  EFI_DEVICE_PATH_PROTOCOL* SelfDevicePath = NULL;
  CONST CHAR16* SelfFilePath = NULL;
  EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
  if (!EFI_ERROR(gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage))) {
    gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID**)&SelfDevicePath);
    SelfFilePath = GetFilePathFromDevicePath(LoadedImage->FilePath);
  }

  // Chainload next boot option
  EFI_STATUS Status = StartDefaultBoot(ImageHandle, SelfDevicePath, SelfFilePath);
  
  // If we reach here, nothing else could be booted
  gBS->Exit(ImageHandle, Status, 0, NULL);
  return EFI_SUCCESS;
}
