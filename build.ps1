# UnderVolter Build Script
# Builds x64 Release, copies to bin, includes UnderVolter.ini

$projectRoot  = $PSScriptRoot
$binDir       = Join-Path $projectRoot "bin"
$vsDir        = Join-Path $projectRoot ".vs"
$slnPath      = Join-Path $projectRoot "UnderVolter.sln"
$srcDir       = Join-Path $projectRoot "src"
$outputDir    = Join-Path $projectRoot "x64\Release"
$embedScript  = Join-Path $projectRoot "Signer\embed-cert.ps1"
$signScript   = Join-Path $projectRoot "Signer\sign.ps1"

# Future timestamp for static build signature
$futureDate = [DateTime]"2030-01-01 00:00:00"

Write-Host "--- UnderVolter Build Script ---" -ForegroundColor Cyan
Write-Host ""

# ============================================================================
# Step 0: Embed certificate into src/UnderVolterCert.h
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

$vsPath = &$vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) {
    Write-Error "Visual Studio Build Tools not found."
    exit 1
}

$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
Write-Host "  MSBuild: $msbuild" -ForegroundColor Gray

# ============================================================================
# Step 2: Clean bin and .vs directories
# ============================================================================
Write-Host ""
Write-Host "--- Cleaning build directories ---" -ForegroundColor Cyan

# Clean bin directory
if (Test-Path $binDir) {
    Remove-Item $binDir -Recurse -Force
}
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
Write-Host "  Cleaned: bin" -ForegroundColor Gray

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
# Step 4: Copy output to bin
# ============================================================================
Write-Host ""
Write-Host "--- Copying to bin ---" -ForegroundColor Cyan

# Copy EFI binaries
$efiNames = @("UnderVolter.efi", "Loader.efi")
foreach ($efiName in $efiNames) {
    $efiSource = Join-Path $outputDir $efiName
    if (Test-Path $efiSource) {
        Copy-Item -Path $efiSource -Destination $binDir -Force
        Write-Host "  Copied: $efiName" -ForegroundColor Green
    } else {
        Write-Host "  ERROR: $efiName not found in $outputDir" -ForegroundColor Red
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
        $null = certutil -f -p $pfxPassword -importpfx -user $pfxFile.FullName 2>&1
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

            # Keep the build output tree in sync with the signed artifact.
            # x64\Release\UnderVolter.efi is used by some local boot/debug flows.
            $signedOutputCopy = Join-Path $outputDir "UnderVolter.efi"
            Copy-Item -LiteralPath $efiToSign -Destination $signedOutputCopy -Force
            $signedItem = Get-Item -LiteralPath $signedOutputCopy
            $signedItem.LastWriteTime = $signedItem.CreationTime = $signedItem.LastAccessTime = $futureDate
            Write-Host "  Updated signed copy: x64\\Release\\UnderVolter.efi" -ForegroundColor Green
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

if (Test-Path $outputDir) {
    Get-ChildItem -Path $outputDir | Where-Object {
        $_.Name -notlike "*.efi"
    } | Remove-Item -Recurse -Force
    Write-Host "  Cleaned: $outputDir" -ForegroundColor Gray
}

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
