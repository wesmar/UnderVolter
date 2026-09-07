# UnderVolter Build Script
# Builds x64 Release directly into bin and removes intermediate files.

$projectRoot  = $PSScriptRoot
$binDir       = Join-Path $projectRoot "bin"
$vsDir        = Join-Path $projectRoot ".vs"
$slnPath      = Join-Path $projectRoot "UnderVolter.sln"
$srcDir       = Join-Path $projectRoot "src"
$libDir       = Join-Path $projectRoot "lib"
$includeDir   = Join-Path $projectRoot "include"
$buildDir     = Join-Path $projectRoot ".build"
$embedScript  = Join-Path $projectRoot "Signer\embed-cert.ps1"

# Future timestamp for static build signature
$futureDate = [DateTime]"2030-01-01 00:00:00"

Write-Host "--- UnderVolter Build Script ---" -ForegroundColor Cyan
Write-Host ""

# ============================================================================
# Step 0a: Verify self-contained build dependencies
# ============================================================================
$requiredLibs = @(
    "BaseDebugPrintErrorLevelLib.lib",
    "BaseLib.lib",
    "BasePrintLib.lib",
    "BaseSynchronizationLib.lib",
    "GlueLib.lib",
    "UefiApplicationEntryPoint.lib",
    "UefiBootServicesTableLib.lib",
    "UefiDebugLibConOut.lib",
    "UefiDevicePathLibDevicePathProtocol.lib",
    "UefiFileHandleLib.lib",
    "UefiHiiLib.lib",
    "UefiHiiServicesLib.lib",
    "UefiLib.lib",
    "UefiMemoryAllocationLib.lib",
    "UefiMemoryLib.lib",
    "UefiRuntimeServicesTableLib.lib",
    "UefiShellLib.lib",
    "UefiSortLib.lib"
)

$missingDependencies = @($requiredLibs | Where-Object { -not (Test-Path -LiteralPath (Join-Path $libDir $_)) })
if (-not (Test-Path -LiteralPath (Join-Path $includeDir "vshacks.h"))) {
    $missingDependencies += "include\vshacks.h"
}
if ($missingDependencies.Count -gt 0) {
    Write-Error ("Missing local build dependencies: " + ($missingDependencies -join ", "))
    exit 1
}
Write-Host "  Local vshacks.h and lib/ dependencies are complete" -ForegroundColor Green
Write-Host ""

# ============================================================================
# Step 0b: Embed certificate into src/UnderVolterCert.h
# ============================================================================
Write-Host "--- Embedding certificate ---" -ForegroundColor Cyan
& powershell.exe -ExecutionPolicy Bypass -File $embedScript
if ($LASTEXITCODE -ne 0) {
    Write-Host "  WARNING: embed-cert.ps1 failed - UnderVolterCert.h may be stale." -ForegroundColor Yellow
}
Write-Host ""

# ============================================================================
# Step 1: Find MSBuild
# ============================================================================
Write-Host "--- Finding Visual Studio Build Tools ---" -ForegroundColor Cyan

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Install Visual Studio 2022 or later."
    exit 1
}

$instances = & $vswhere -all -products * -prerelease -format json | ConvertFrom-Json
$msbuild = $null
$toolset = $null
foreach ($instance in ($instances | Sort-Object { [version]$_.installationVersion } -Descending)) {
    $candidateMsbuild = Join-Path $instance.installationPath "MSBuild\Current\Bin\MSBuild.exe"
    $candidateToolset = Get-ChildItem -LiteralPath (Join-Path $instance.installationPath "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName "bin\Hostx64\x64\ml64.exe")) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName "bin\Hostx64\x64\cl.exe"))
        } | Select-Object -First 1
    if ((Test-Path -LiteralPath $candidateMsbuild) -and $candidateToolset) {
        $msbuild = $candidateMsbuild
        $toolset = $candidateToolset.FullName
        break
    }
}
if (-not $msbuild) {
    Write-Error "No Visual Studio installation with MSBuild, cl.exe, and ml64.exe for x64 was found."
    exit 1
}
Write-Host "  MSBuild: $msbuild" -ForegroundColor Gray
Write-Host "  MSVC:    $toolset" -ForegroundColor Gray

# ============================================================================
# Step 2: Clean output and intermediate directories
# ============================================================================
Write-Host ""
Write-Host "--- Cleaning build directories ---" -ForegroundColor Cyan

# Clean bin directory
if (Test-Path $binDir) {
    Remove-Item $binDir -Recurse -Force
}
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
Write-Host "  Cleaned: bin" -ForegroundColor Gray

if (Test-Path $buildDir) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}
Write-Host "  Cleaned: .build" -ForegroundColor Gray

foreach ($legacyDir in @((Join-Path $projectRoot "x64"), (Join-Path $srcDir "x64"))) {
    $resolvedLegacy = [IO.Path]::GetFullPath($legacyDir)
    $resolvedRoot = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\') + '\'
    if ($resolvedLegacy.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedLegacy)) {
        Remove-Item -LiteralPath $resolvedLegacy -Recurse -Force
        Write-Host "  Removed legacy output: $resolvedLegacy" -ForegroundColor Gray
    }
}

# Clean .vs directory (Visual Studio cache)
if (Test-Path $vsDir) {
    Remove-Item $vsDir -Recurse -Force
    Write-Host "  Cleaned: .vs" -ForegroundColor Gray
} else {
    Write-Host "  .vs not found - skipping" -ForegroundColor Gray
}

# ============================================================================
# Step 3: Build Release x64
# ============================================================================
Write-Host ""
Write-Host "--- Building Release x64 ---" -ForegroundColor Cyan

$msbuildArgs = @(
    $slnPath,
    "/t:Rebuild",
    "/p:Configuration=Release",
    "/p:Platform=x64",
    "/m",
    "/nologo",
    "/v:m"
)

Write-Host "  Building: $slnPath" -ForegroundColor Gray
&$msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "!!! BUILD FAILED !!!" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "  Build completed successfully" -ForegroundColor Green

# ============================================================================
# Step 4: Verify output and add configuration
# ============================================================================
Write-Host ""
Write-Host "--- Finalizing bin ---" -ForegroundColor Cyan

# Verify EFI binaries emitted directly by the projects.
$efiNames = @("UnderVolter.efi", "Loader.efi")
foreach ($efiName in $efiNames) {
    $efiOutput = Join-Path $binDir $efiName
    if (Test-Path -LiteralPath $efiOutput) {
        Write-Host "  Built: $efiName" -ForegroundColor Green
    } else {
        Write-Host "  ERROR: $efiName was not emitted into $binDir" -ForegroundColor Red
        exit 1
    }
}

# Copy UnderVolter.ini
$iniSource = Join-Path $srcDir "UnderVolter.ini"
if (Test-Path $iniSource) {
    Copy-Item -Path $iniSource -Destination $binDir -Force
    Write-Host "  Copied: UnderVolter.ini" -ForegroundColor Green
} else {
    Write-Host "  WARNING: UnderVolter.ini not found in $srcDir" -ForegroundColor Yellow
}

# Set future timestamp on all files in bin (2030-01-01 00:00:00)
Write-Host "  Setting file dates to 2030-01-01 00:00:00" -ForegroundColor Gray
Get-ChildItem -Path $binDir | ForEach-Object {
    $_.LastWriteTime = $futureDate
    $_.CreationTime = $futureDate
    $_.LastAccessTime = $futureDate
}

# ============================================================================
# Step 4b: Sign UnderVolter.efi (if certificate is available)
# ============================================================================
Write-Host ""
Write-Host "--- Signing UnderVolter.efi ---" -ForegroundColor Cyan

$signerCertDir = Join-Path $projectRoot "Signer\cert"
$pfxFile  = Get-ChildItem -Path $signerCertDir -Filter "*-standard-signing.pfx" -ErrorAction SilentlyContinue | Select-Object -First 1
$pwdFile  = Get-ChildItem -Path $signerCertDir -Filter "*-standard-signing.pwd" -ErrorAction SilentlyContinue | Select-Object -First 1
$efiToSign = Join-Path $binDir "UnderVolter.efi"

if ($pfxFile -and $pwdFile) {
    # Locate signtool.exe from the newest installed Windows Kit
    $kitsRoot    = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $signToolExe = Get-ChildItem -Path $kitsRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1

    if (-not $signToolExe) {
        Write-Host "  WARNING: signtool.exe not found - EFI is unsigned." -ForegroundColor Yellow
    } else {
        $pfxPassword = (Get-Content -LiteralPath $pwdFile.FullName -Raw).Trim()
        Write-Host "  SignTool: $signToolExe" -ForegroundColor Gray

        # Import PFX into CurrentUser\My cert store so signtool can use it.
        # (signtool cannot use openssl PFX directly due to CryptoAPI compatibility.)
        $null = certutil -silent -f -p $pfxPassword -importpfx -user $pfxFile.FullName 2>&1
        $pfxPassword = $null

        Write-Host "  Signing UnderVolter.efi..." -ForegroundColor Cyan
        # /s My: search CurrentUser\My store  /n: match by subject CN
        # No /ph (page hash) -- EFI section alignment is 32 bytes, not page-aligned
        & $signToolExe sign /fd sha256 /n "UnderVolter" /s My $efiToSign
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  WARNING: signtool.exe failed - bin\UnderVolter.efi is unsigned." -ForegroundColor Yellow
        } else {
            # Reset timestamp after signtool stamps it
            $item = Get-Item -LiteralPath $efiToSign
            $item.LastWriteTime = $item.CreationTime = $item.LastAccessTime = $futureDate
            Write-Host "  Signed: UnderVolter.efi" -ForegroundColor Green

        }
    }
} else {
    Write-Host "  No signing certificate found - UnderVolter.efi is unsigned." -ForegroundColor Yellow
    Write-Host "  To enable SelfEnroll: .\Signer\sign.ps1 -Create  then  .\build.ps1" -ForegroundColor DarkGray
}

# ============================================================================
# Step 5: Clean build output
# ============================================================================
Write-Host ""
Write-Host "--- Cleaning build output ---" -ForegroundColor Cyan

if (Test-Path $buildDir) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
    Write-Host "  Removed: .build" -ForegroundColor Gray
}

Get-ChildItem -LiteralPath $binDir -File | Where-Object { $_.Extension -notin @(".efi", ".ini") } |
    Remove-Item -Force

# ============================================================================
# Summary
# ============================================================================
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "BUILD COMPLETED SUCCESSFULLY" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Output directory: $binDir" -ForegroundColor White
Get-ChildItem -Path $binDir | ForEach-Object {
    Write-Host "  - $($_.Name) ($($_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')))" -ForegroundColor Gray
}
Write-Host ""
