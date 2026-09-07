<#
.SYNOPSIS
    QEMU runner for the UnderVolter project.

.DESCRIPTION
    Runs UnderVolter.efi or Loader.efi in QEMU with EDK2/OVMF x86_64 firmware,
    optional SMM and Secure Boot, a high-resolution GOP display, and selectable
    Intel CPUID profiles.

.PARAMETER Cpu
    CPU profile used for CPUID emulation:
    - CoffeeLake (default, i7-9750H - family=6, model=158, stepping=10)
    - CometLake  (i9-10900K - family=6, model=165, stepping=2)
    - RocketLake (i9-11900K - family=6, model=167, stepping=1)
    - AlderLake  (i9-12900K - family=6, model=151, stepping=2)
    - RaptorLake (i9-13900K - family=6, model=183, stepping=1)
    - ArrowLake  (Core Ultra 200S - family=6, model=197, stepping=2)
    - Skylake    (Skylake-Client-v4)
    - Max        (QEMU max CPU)

.PARAMETER Target
    Binary to run:
    - UnderVolter (default: direct UnderVolter.efi boot)
    - Loader      (Loader.efi chainloader test)
    - Shell       (UEFI Shell without application autostart)

.PARAMETER Res
    Virtual BIOS/GOP display resolution:
    - Native   (matches the primary display, for example 1280x800)
    - 1080p    (1920x1080 - Full HD, default)
    - 1440p/2K (2560x1440)
    - 4K       (3840x2160)
    - 1024x768 (classic BIOS resolution)
    Any WxH value is also accepted, for example -Res 1920x1200.

.PARAMETER FullScreen
    Runs the emulator in full-screen mode.

.PARAMETER ZoomToFit
    GTK scaling. The default is "off" for a sharp 1:1 image; "on" fits the window.

.PARAMETER SecureBoot
    Enables Secure Boot and SMM with edk2-x86_64-secure-code.fd.

.PARAMETER ResetNvram
    Resets the NVRAM store from the edk2-i386-vars.fd template.

.PARAMETER Cores
    Number of emulated logical processors (default: 4).

.PARAMETER Memory
    Virtual machine memory (default: 1024M).

.EXAMPLE
    .\run-qemu.ps1                          # Start UnderVolter with the default profile
    .\run-qemu.ps1 -FullScreen              # Full-screen mode
    .\run-qemu.ps1 -Res 1440p               # 2560x1440 resolution
    .\run-qemu.ps1 -Res Native              # Match the primary display
    .\run-qemu.ps1 -Cpu AlderLake           # Emulate a 12th-generation CPUID
    .\run-qemu.ps1 -SecureBoot              # Test Secure Boot and SMM
#>

[CmdletBinding()]
param(
    [string]$Cpu = "CoffeeLake",
    [ValidateSet("UnderVolter", "Loader", "Shell")]
    [string]$Target = "UnderVolter",
    [string]$Res = "",
    [switch]$FullScreen,
    [int]$Width = 0,
    [int]$Height = 0,
    [ValidateSet("on", "off")]
    [string]$ZoomToFit = "off",
    [switch]$SecureBoot,
    [switch]$ResetNvram,
    [int]$Cores = 4,
    [string]$Memory = "1024M",
    [string]$QemuDir = "C:\Program Files\qemu",
    [string]$VmDir = "",
    [switch]$Console,
    [switch]$AutoBuild,
    [switch]$Headless,
    [switch]$NoReboot,
    [ValidateRange(0, 65535)]
    [int]$QmpPort = 0,
    [string]$BinaryDir = "",
    [string]$IniPath = ""
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

# ==============================================================================
# 1. Resolve GOP/BIOS display resolution
# ==============================================================================
$screenW = 1280
$screenH = 800
try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
    $primary = [System.Windows.Forms.Screen]::PrimaryScreen
    if ($primary) {
        $screenW = $primary.Bounds.Width
        $screenH = $primary.Bounds.Height
    }
} catch {}

if ($Res) {
    switch -Regex ($Res.ToLower().Trim()) {
        "^native$"      { $Width = $screenW; $Height = $screenH }
        "^(1080p|fhd)$" { $Width = 1920; $Height = 1080 }
        "^(1440p|2k|qhd)$" { $Width = 2560; $Height = 1440 }
        "^(4k|uhd|2160p)$" { $Width = 3840; $Height = 2160 }
        "^1024x768$"    { $Width = 1024; $Height = 768 }
        "^(\d+)x(\d+)$" {
            $Width  = [int]$Matches[1]
            $Height = [int]$Matches[2]
        }
        Default {
            Write-Warning "Unknown resolution preset '$Res'; using 1920x1080."
            $Width = 1920; $Height = 1080
        }
    }
}

if ($Width -le 0 -or $Height -le 0) {
    if ($FullScreen) {
        $Width = $screenW
        $Height = $screenH
    } else {
        # Use native resolution on smaller displays to keep a readable 1:1 window.
        if ($screenW -le 1366) {
            $Width = $screenW
            $Height = $screenH
        } else {
            $Width = 1920
            $Height = 1080
        }
    }
}

# ==============================================================================
# 2. Base paths and VM working directory
# ==============================================================================
$ScriptRoot = $PSScriptRoot
if (-not $ScriptRoot) { $ScriptRoot = (Get-Location).Path }

if (-not $VmDir) {
    $VmDir = Join-Path $ScriptRoot ".qemu-vm"
} else {
    $VmDir = [System.IO.Path]::GetFullPath($VmDir)
}

$EspDir      = Join-Path $VmDir "esp"
$FirmwareDir = Join-Path $VmDir "firmware"
$LogDir      = Join-Path $VmDir "logs"

foreach ($dir in @($VmDir, $EspDir, $FirmwareDir, $LogDir)) {
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
}

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "               UnderVolter — Professional QEMU Runner                 " -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

# ==============================================================================
# 3. Locate and verify QEMU
# ==============================================================================
$CandidateQemuDirs = @(
    $QemuDir,
    "C:\Program Files\qemu",
    "C:\PROGRA~1\qemu",
    "${env:ProgramFiles}\qemu",
    "${env:ProgramFiles(x86)}\qemu"
)

$ResolvedQemuDir = $null
foreach ($cand in $CandidateQemuDirs) {
    if ($cand -and (Test-Path (Join-Path $cand 'qemu-system-x86_64.exe'))) {
        $ResolvedQemuDir = [System.IO.Path]::GetFullPath($cand)
        break
    }
}

if (-not $ResolvedQemuDir) {
    $qemuInPath = Get-Command "qemu-system-x86_64.exe" -ErrorAction SilentlyContinue
    if ($qemuInPath) {
        $ResolvedQemuDir = Split-Path -Parent $qemuInPath.Source
    }
}

if (-not $ResolvedQemuDir -or -not (Test-Path $ResolvedQemuDir)) {
    Write-Error "QEMU was not found in '$QemuDir' or PATH. Pass -QemuDir to select an installation."
    exit 1
}

$exeName = if ($Console) { "qemu-system-x86_64.exe" } else { "qemu-system-x86_64w.exe" }
$qemuExe = Join-Path $ResolvedQemuDir $exeName
if (-not (Test-Path $qemuExe)) {
    $qemuExe = Join-Path $ResolvedQemuDir "qemu-system-x86_64.exe"
}

if (-not (Test-Path $qemuExe)) {
    Write-Error "QEMU executable was not found: $qemuExe"
    exit 1
}

$qemuShareDir = Join-Path $ResolvedQemuDir "share"
if (-not (Test-Path $qemuShareDir)) {
    Write-Error "QEMU share directory was not found: $qemuShareDir"
    exit 1
}

$qemuVersion = "unknown"
$versionFile = Join-Path $ResolvedQemuDir "VERSION"
if (Test-Path $versionFile) {
    $qemuVersion = (Get-Content $versionFile -Raw).Trim()
}

Write-Host "  QEMU Binary  : " -NoNewline -ForegroundColor Gray
Write-Host "$qemuExe (v$qemuVersion)" -ForegroundColor Green

# ==============================================================================
# 4. Prepare EDK2/OVMF firmware
# ==============================================================================
$sysVarsTemplate = Join-Path $qemuShareDir "edk2-i386-vars.fd"
if (-not (Test-Path $sysVarsTemplate)) {
    Write-Error "QEMU NVRAM template is missing: $sysVarsTemplate"
    exit 1
}

$sysCodeFd = $null
$localCodeName = ""
$localVarsName = ""

if ($SecureBoot) {
    $sysCodeFd = Join-Path $qemuShareDir "edk2-x86_64-secure-code.fd"
    $localCodeName = "ovmf-x86_64-secure-code.fd"
    $localVarsName = "ovmf-x86_64-secure-vars.fd"
    Write-Host "  Firmware Type: " -NoNewline -ForegroundColor Gray
    Write-Host "Secure Boot + SMM (edk2-x86_64-secure-code.fd)" -ForegroundColor Yellow
} else {
    $sysCodeFd = Join-Path $qemuShareDir "edk2-x86_64-code.fd"
    $localCodeName = "ovmf-x86_64-code.fd"
    $localVarsName = "ovmf-x86_64-vars.fd"
    Write-Host "  Firmware Type: " -NoNewline -ForegroundColor Gray
    Write-Host "Standard UEFI (edk2-x86_64-code.fd)" -ForegroundColor Green
}

if (-not (Test-Path $sysCodeFd)) {
    Write-Error "QEMU firmware image was not found: $sysCodeFd"
    exit 1
}

# Copy firmware into the VM directory to avoid path quoting issues.
$localCodeFd = Join-Path $FirmwareDir $localCodeName
$localVarsFd = Join-Path $FirmwareDir $localVarsName

if (-not (Test-Path $localCodeFd) -or (Get-Item $sysCodeFd).LastWriteTime -gt (Get-Item $localCodeFd).LastWriteTime) {
    Write-Host "  Updating the local CODE.fd image from QEMU..." -ForegroundColor Gray
    Copy-Item -LiteralPath $sysCodeFd -Destination $localCodeFd -Force
}

if ($ResetNvram -or -not (Test-Path $localVarsFd)) {
    Write-Host "  Initializing a clean NVRAM variable store (VARS.fd)..." -ForegroundColor Yellow
    Copy-Item -LiteralPath $sysVarsTemplate -Destination $localVarsFd -Force
}

# ==============================================================================
# 5. Prepare UnderVolter binaries
# ==============================================================================
$binDir = if ($BinaryDir) { [IO.Path]::GetFullPath($BinaryDir) } else { Join-Path $ScriptRoot "bin" }
$srcUv  = Join-Path $binDir "UnderVolter.efi"
$srcLd  = Join-Path $binDir "Loader.efi"
$srcIni = if ($IniPath) { [IO.Path]::GetFullPath($IniPath) } else { Join-Path $binDir "UnderVolter.ini" }

if ($AutoBuild -or (-not (Test-Path $srcUv))) {
    Write-Host "`n[*] Starting automatic build (build.ps1)..." -ForegroundColor Cyan
    $buildScript = Join-Path $ScriptRoot "build.ps1"
    & powershell.exe -ExecutionPolicy Bypass -File $buildScript
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Automatic build failed."
        exit $LASTEXITCODE
    }
}

if (-not (Test-Path $srcUv)) {
    Write-Error "$srcUv is missing. Build the project with .\build.ps1."
    exit 1
}

# Copy application files to the virtual ESP.
Copy-Item -LiteralPath $srcUv -Destination (Join-Path $EspDir "undervolter.efi") -Force
if (Test-Path $srcLd)  { Copy-Item -LiteralPath $srcLd  -Destination (Join-Path $EspDir "loader.efi") -Force }
if (Test-Path $srcIni) { Copy-Item -LiteralPath $srcIni -Destination (Join-Path $EspDir "UnderVolter.ini") -Force }
else { Remove-Item -LiteralPath (Join-Path $EspDir "UnderVolter.ini") -ErrorAction SilentlyContinue }

# Create the default UEFI EFI\BOOT path.
$bootDir = Join-Path $EspDir "EFI\BOOT"
if (-not (Test-Path $bootDir)) { New-Item -ItemType Directory -Force -Path $bootDir | Out-Null }
$bootX64 = Join-Path $bootDir "BOOTX64.EFI"

if ($Headless) {
    $targetBinary = if ($Target -eq "Loader" -and (Test-Path $srcLd)) { $srcLd } else { $srcUv }
    Copy-Item -LiteralPath $targetBinary -Destination $bootX64 -Force
} else {
    # In interactive mode, let OVMF start the UEFI Shell and startup.nsh.
    # The shell remains open after the application returns.
    if (Test-Path $bootX64) {
        Remove-Item -LiteralPath $bootX64 -Force -ErrorAction SilentlyContinue
    }
}

# Create startup.nsh.
$startupNshPath = Join-Path $EspDir "startup.nsh"
$startupLines = [System.Collections.Generic.List[string]]::new()
$startupLines.Add("@echo -off")
$startupLines.Add("cls")

switch ($Target) {
    "UnderVolter" {
        $startupLines.Add("fs0:")
        $startupLines.Add("undervolter.efi")
        if (-not $Headless) {
            $startupLines.Add("echo .")
            $startupLines.Add("echo === Done. Press Enter to close QEMU ===")
            $startupLines.Add("pause")
        }
    }
    "Loader" {
        $startupLines.Add("fs0:")
        $startupLines.Add("loader.efi")
        if (-not $Headless) {
            $startupLines.Add("echo .")
            $startupLines.Add("echo === Done. Press Enter to close QEMU ===")
            $startupLines.Add("pause")
        }
    }
    "Shell" {
        $startupLines.Add("echo [Shell mode: startup.nsh does not start an application.]")
        $startupLines.Add("echo Run 'undervolter.efi' to begin.")
    }
}

Set-Content -LiteralPath $startupNshPath -Value ($startupLines -join "`r`n") -Encoding ASCII

# ==============================================================================
# 6. Configure the emulated CPU profile
# ==============================================================================
$CpuArg = ""
$CpuDesc = ""

$AvxFeatures = "+avx,+avx2,+fma,+bmi1,+bmi2,+smep,+smap,+xsave,+xsaveopt"

switch ($Cpu) {
    "CoffeeLake" {
        $CpuArg  = "qemu64,family=6,model=158,stepping=10,$AvxFeatures"
        $CpuDesc = "Coffee Lake Core i7-9750H (Family 6, Model 158, Stepping 10)"
    }
    "CometLake" {
        $CpuArg  = "qemu64,family=6,model=165,stepping=2,$AvxFeatures"
        $CpuDesc = "Comet Lake Core i9-10900K (Family 6, Model 165, Stepping 2)"
    }
    "RocketLake" {
        $CpuArg  = "qemu64,family=6,model=167,stepping=1,$AvxFeatures"
        $CpuDesc = "Rocket Lake Core i9-11900K (Family 6, Model 167, Stepping 1)"
    }
    "AlderLake" {
        $CpuArg  = "qemu64,family=6,model=151,stepping=2,$AvxFeatures"
        $CpuDesc = "Alder Lake Core i9-12900K (Family 6, Model 151, Stepping 2)"
    }
    "RaptorLake" {
        $CpuArg  = "qemu64,family=6,model=183,stepping=1,$AvxFeatures"
        $CpuDesc = "Raptor Lake Core i9-13900K (Family 6, Model 183, Stepping 1)"
    }
    "ArrowLake" {
        $CpuArg  = "qemu64,family=6,model=197,stepping=2,$AvxFeatures"
        $CpuDesc = "Arrow Lake Core Ultra 200S (Family 6, Model 197, Stepping 2)"
    }
    "Skylake" {
        $CpuArg  = "Skylake-Client-v4"
        $CpuDesc = "Intel Skylake Client (Skylake-Client-v4)"
    }
    "Max" {
        $CpuArg  = "max"
        $CpuDesc = "Host Maximum Capabilities (max)"
    }
    Default {
        $CpuArg  = $Cpu
        $CpuDesc = "Custom: $Cpu"
    }
}

Write-Host "  Emulated CPU : " -NoNewline -ForegroundColor Gray
Write-Host "$CpuDesc" -ForegroundColor Green
Write-Host "  RAM / Cores  : " -NoNewline -ForegroundColor Gray
Write-Host "$Memory / $Cores logical cores" -ForegroundColor Green
Write-Host "  GOP Display  : " -NoNewline -ForegroundColor Gray
Write-Host "${Width}x${Height} (Bochs Adapter, FullScreen: $FullScreen, Zoom: $ZoomToFit)" -ForegroundColor Green
Write-Host "  Target Mode  : " -NoNewline -ForegroundColor Gray
Write-Host "$Target (Direct Fast Boot)" -ForegroundColor Green
Write-Host "  VM Directory : " -NoNewline -ForegroundColor Gray
Write-Host "$VmDir`n" -ForegroundColor DarkGray

# ==============================================================================
# 7. Compose QEMU command-line arguments
# ==============================================================================
$qemuArgs = [System.Collections.Generic.List[string]]::new()

# Q35 chipset and machine
if ($SecureBoot) {
    $qemuArgs.Add("-machine")
    $qemuArgs.Add("q35,smm=on")
    $qemuArgs.Add("-global")
    $qemuArgs.Add("driver=cfi.pflash01,property=secure,value=on")
    $qemuArgs.Add("-global")
    $qemuArgs.Add("ICH9-LPC.disable_s3=1")
} else {
    $qemuArgs.Add("-M")
    $qemuArgs.Add("q35")
}

# CPU, memory, and logical processors
$qemuArgs.Add("-cpu")
$qemuArgs.Add($CpuArg)
$qemuArgs.Add("-m")
$qemuArgs.Add($Memory)
$qemuArgs.Add("-smp")
$qemuArgs.Add($Cores.ToString())

# PFLASH: read-only CODE and writable VARS
$qemuArgs.Add("-drive")
$qemuArgs.Add("if=pflash,format=raw,readonly=on,file=$localCodeFd")
$qemuArgs.Add("-drive")
$qemuArgs.Add("if=pflash,format=raw,file=$localVarsFd")

# Virtual SATA disk containing EFI files and UnderVolter.ini.
$qemuArgs.Add("-drive")
$qemuArgs.Add("file=fat:rw:$EspDir,format=raw,media=disk")

# GOP display through Bochs Display Adapter with OVMF resolution hints.
$qemuArgs.Add("-vga")
$qemuArgs.Add("none")
$qemuArgs.Add("-device")
$qemuArgs.Add("bochs-display,xres=$Width,yres=$Height")
$qemuArgs.Add("-fw_cfg")
$qemuArgs.Add("name=opt/ovmf/X-Resolution,string=$Width")
$qemuArgs.Add("-fw_cfg")
$qemuArgs.Add("name=opt/ovmf/Y-Resolution,string=$Height")

# GTK display options
$displayOpts = "gtk,zoom-to-fit=$ZoomToFit,show-menubar=off,show-tabs=off,window-close=on"
if ($FullScreen) {
    $displayOpts += ",full-screen=on"
}
$qemuArgs.Add("-display")
$qemuArgs.Add($(if ($Headless) { 'none' } else { $displayOpts }))
if ($QmpPort) {
    $qemuArgs.Add('-qmp')
    $qemuArgs.Add("tcp:127.0.0.1:$QmpPort,server=on,wait=off")
}
if ($NoReboot) { $qemuArgs.Add('-no-reboot') }

# COM1 serial output to a diagnostic log
$serialLogFile = Join-Path $LogDir "uefi-serial.log"
$qemuArgs.Add("-serial")
$qemuArgs.Add("file:$serialLogFile")

# Additional options
$qemuArgs.Add("-net")
$qemuArgs.Add("none")
$qemuArgs.Add("-name")
$qemuArgs.Add("UnderVolter-QEMU-$Cpu")

# Standard-error log
$errFile = Join-Path $LogDir "qemu-error.log"
Remove-Item -LiteralPath $errFile -ErrorAction SilentlyContinue

Write-Host "Starting QEMU..." -ForegroundColor Green
Write-Host "Close the emulator window (or press Alt+F4) to end the session.`n" -ForegroundColor DarkGray

# ==============================================================================
# 8. Run QEMU and wait for it to exit
# ==============================================================================
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Start-Process joins arguments; quote each one so VM paths may contain spaces.
$quotedArgs = $qemuArgs | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
$windowStyle = if ($Headless) { "Hidden" } else { "Normal" }
$proc = Start-Process -FilePath $qemuExe -ArgumentList $quotedArgs -PassThru -WindowStyle $windowStyle -RedirectStandardError $errFile
$proc.Id | Set-Content -LiteralPath (Join-Path $VmDir 'qemu.pid') -Encoding ASCII
$proc.WaitForExit()
$exitCode = $proc.ExitCode

$sw.Stop()

# ==============================================================================
# 9. Session summary
# ==============================================================================
Write-Host "`n--- QEMU session ended ---" -ForegroundColor Cyan
Write-Host "  Session duration : $($sw.Elapsed.TotalSeconds.ToString('F1')) s" -ForegroundColor Gray
Write-Host "  Exit code        : $exitCode" -ForegroundColor $(if ($exitCode -eq 0) { "Green" } else { "Red" })

if (Test-Path $errFile) {
    $errContent = Get-Content -LiteralPath $errFile | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    if ($errContent) {
        Write-Host "`nQEMU diagnostics and errors:" -ForegroundColor Yellow
        $errContent | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }
}

if (Test-Path $serialLogFile) {
    Write-Host "  UEFI console log : $serialLogFile" -ForegroundColor DarkGray
}
