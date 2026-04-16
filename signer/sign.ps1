# sign.ps1 - Generate root CA + signing certificate for UnderVolter, and/or
#             sign UnderVolter.efi with Authenticode (PE/COFF, SHA-256).
#
# UEFI Secure Boot uses Authenticode signatures embedded in EFI binaries.
# The root CA certificate is enrolled into db, KEK, and PK by SelfEnroll.
# Any future UnderVolter.efi signed with the leaf signing cert is trusted
# without re-enrolling BIOS keys.
#
# Usage:
#   .\sign.ps1 -Create              Generate certificate set (run once)
#   .\sign.ps1 -Create -Force       Recreate (replaces existing certs)
#   .\sign.ps1                      Sign bin\UnderVolter.efi
#   .\sign.ps1 -Name "My Name"      Use custom certificate subject name
#
# After -Create: rebuilds src/UnderVolterCert.h and prompts to rebuild.
# After signing: updates bin\UnderVolter.efi with embedded signature.

[CmdletBinding()]
param(
    [switch]$Create,
    [string]$Name,
    [string]$TargetPath,
    [string]$Timestamp = "2030-01-01 00:00:00",
    [switch]$Force
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

$SignerRoot  = $PSScriptRoot
$ProjectRoot = Split-Path -Parent $SignerRoot
$CertDir     = Join-Path $SignerRoot "cert"
$BinDir      = Join-Path $ProjectRoot "bin"
$ConfigPath  = Join-Path $CertDir "signing.config.json"
$EmbedScript = Join-Path $SignerRoot "embed-cert.ps1"

$DefaultTarget = Join-Path $BinDir "UnderVolter.efi"

function Write-Info([string]$m)    { Write-Host $m -ForegroundColor Cyan }
function Write-Step([string]$m)    { Write-Host $m -ForegroundColor DarkGray }
function Write-Success([string]$m) { Write-Host $m -ForegroundColor Green }
function Write-Warn([string]$m)    { Write-Host $m -ForegroundColor Yellow }
function Write-Fail([string]$m)    { Write-Host $m -ForegroundColor Red }

# ── Helpers ───────────────────────────────────────────────────────────────────

function New-PasswordString([int]$Length = 40) {
    $alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^*_-+="
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $Length; $i++) {
        [void]$sb.Append($alphabet[(Get-Random -Minimum 0 -Maximum $alphabet.Length)])
    }
    return $sb.ToString()
}

function Read-Password([string]$Path) {
    return (Get-Content -LiteralPath $Path -Raw).Trim()
}

function Parse-Timestamp([string]$Value) {
    try {
        return [datetime]::Parse(
            $Value,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::AllowWhiteSpaces -bor
            [System.Globalization.DateTimeStyles]::AssumeLocal)
    } catch { throw "Invalid -Timestamp '$Value'. Example: 2030-01-01 00:00:00" }
}

function Set-FileTimestamp([string[]]$Paths, [datetime]$Value) {
    foreach ($p in $Paths) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $item = Get-Item -LiteralPath $p
        $item.CreationTime = $item.LastWriteTime = $item.LastAccessTime = $Value
    }
}

function Get-SignTool {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (-not (Test-Path -LiteralPath $kitsRoot)) { throw "Windows Kits 10 not found." }

    $tool = Get-ChildItem -LiteralPath $kitsRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1

    if ($tool) { return $tool }
    throw "signtool.exe not found in Windows Kits."
}

# ── Config ────────────────────────────────────────────────────────────────────

function Load-Config {
    $default = [ordered]@{ Name = "UnderVolter" }
    if (-not (Test-Path -LiteralPath $ConfigPath)) { return $default }
    try {
        $obj = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
        if ($obj.Name -and -not [string]::IsNullOrWhiteSpace($obj.Name)) {
            return [ordered]@{ Name = $obj.Name }
        }
    } catch { }
    return $default
}

function Save-Config([string]$CertName) {
    @{ Name = $CertName } | ConvertTo-Json | Set-Content -LiteralPath $ConfigPath -Encoding ASCII
}

function Get-Slug([string]$Value) {
    $slug = ($Value -replace '[^A-Za-z0-9]+', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($slug)) { throw "Certificate name produced an empty slug." }
    return $slug
}

function Get-CertPaths([string]$CertName) {
    $slug = Get-Slug $CertName
    return [ordered]@{
        Name            = $CertName
        Slug            = $slug
        RootSubject     = "CN=$CertName Root CA"
        SignerSubject   = "CN=$CertName"
        RootCerPath     = Join-Path $CertDir "$slug-standard-root.cer"
        SignerCerPath   = Join-Path $CertDir "$slug-standard-signing.cer"
        PfxPath         = Join-Path $CertDir "$slug-standard-signing.pfx"
        PasswordPath    = Join-Path $CertDir "$slug-standard-signing.pwd"
    }
}

function Remove-CertByThumbprint([string]$Thumbprint) {
    foreach ($store in @("Cert:\CurrentUser\My\$Thumbprint", "Cert:\CurrentUser\Root\$Thumbprint")) {
        if (Test-Path -LiteralPath $store) {
            Remove-Item -LiteralPath $store -DeleteKey -Force -ErrorAction SilentlyContinue
        }
    }
}

# ── Certificate creation ──────────────────────────────────────────────────────

function New-CertSet([hashtable]$Paths) {
    if (-not (Test-Path -LiteralPath $CertDir)) {
        New-Item -ItemType Directory -Path $CertDir | Out-Null
    }

    $required = @($Paths.RootCerPath, $Paths.SignerCerPath, $Paths.PfxPath, $Paths.PasswordPath)
    $existing = @($required | Where-Object { Test-Path -LiteralPath $_ })

    if ((-not $Force) -and ($existing.Count -eq $required.Count)) {
        throw "Certificate set already exists. Use -Force to recreate."
    }

    foreach ($p in $existing) { Remove-Item -LiteralPath $p -Force }

    $pwd        = New-PasswordString
    $rootCert   = $null
    $signerCert = $null

    try {
        Write-Info "  Creating root CA certificate..."
        $rootCert = New-SelfSignedCertificate `
            -Type Custom `
            -Subject $Paths.RootSubject `
            -FriendlyName "$($Paths.Name) Root CA" `
            -KeyAlgorithm RSA `
            -KeyLength 4096 `
            -HashAlgorithm sha256 `
            -KeyExportPolicy Exportable `
            -KeyUsage CertSign, CRLSign, DigitalSignature `
            -KeyUsageProperty Sign `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -NotAfter (Get-Date).AddYears(10) `
            -TextExtension @("2.5.29.19={critical}{text}CA=true&pathlength=1")

        Write-Info "  Creating leaf signing certificate..."
        $signerCert = New-SelfSignedCertificate `
            -Type Custom `
            -Subject $Paths.SignerSubject `
            -FriendlyName "$($Paths.Name) Signing" `
            -KeyAlgorithm RSA `
            -KeyLength 4096 `
            -HashAlgorithm sha256 `
            -KeyExportPolicy Exportable `
            -KeySpec Signature `
            -KeyUsage DigitalSignature `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -Signer $rootCert `
            -NotAfter (Get-Date).AddYears(5) `
            -TextExtension @(
                "2.5.29.19={critical}{text}CA=false",
                "2.5.29.37={text}1.3.6.1.5.5.7.3.3"   # EKU: Code Signing
            )

        Export-Certificate -Cert $rootCert   -FilePath $Paths.RootCerPath   -Type CERT | Out-Null
        Export-Certificate -Cert $signerCert -FilePath $Paths.SignerCerPath -Type CERT | Out-Null
        $securePwd = ConvertTo-SecureString -String $pwd -AsPlainText -Force
        Export-PfxCertificate -Cert $signerCert -FilePath $Paths.PfxPath `
            -Password $securePwd -ChainOption BuildChain | Out-Null
        $securePwd = $null

        # Store password as plain text -- the .pfx next to it offers the real protection.
        Set-Content -LiteralPath $Paths.PasswordPath -Value $pwd -Encoding ASCII -NoNewline

        Set-FileTimestamp -Paths $required -Value $script:FixedTs
        Write-Success "  Certificate set created: $($Paths.Name)"
    }
    finally {
        if ($signerCert) { Remove-CertByThumbprint $signerCert.Thumbprint }
        if ($rootCert)   { Remove-CertByThumbprint $rootCert.Thumbprint }
        $pwd = $null
    }
}

# ── Signing ───────────────────────────────────────────────────────────────────

function Assert-CertSet([hashtable]$Paths) {
    $required = @($Paths.RootCerPath, $Paths.SignerCerPath, $Paths.PfxPath, $Paths.PasswordPath)
    foreach ($p in $required) {
        if (-not (Test-Path -LiteralPath $p)) {
            throw "Missing certificate file: $p`nRun: .\Signer\sign.ps1 -Create"
        }
    }
}

function Sign-Efi([hashtable]$Paths, [string]$TargetFile, [string]$SignTool) {
    $plainPwd = Read-Password $Paths.PasswordPath

    try {
        Write-Info "  Signing $([System.IO.Path]::GetFileName($TargetFile))..."
        & $SignTool sign /fd sha256 /f $Paths.PfxPath /p $plainPwd /ph $TargetFile
        if ($LASTEXITCODE -ne 0) { throw "signtool.exe exited with code $LASTEXITCODE." }
    }
    finally { $plainPwd = $null }

    $sig = Get-AuthenticodeSignature -FilePath $TargetFile
    if (-not $sig.SignerCertificate) { throw "Signature not found in $TargetFile after signing." }
    if ($sig.SignerCertificate.Subject -ne $Paths.SignerSubject) {
        throw "Signer subject mismatch in $TargetFile."
    }
    if ($sig.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        Write-Warn "  Signature status: $($sig.Status) - expected until root CA is trusted on this PC."
    }

    Set-FileTimestamp -Paths @($TargetFile) -Value $script:FixedTs
    Write-Success "  Signed: $TargetFile"
}

# ── Main ──────────────────────────────────────────────────────────────────────

try {
    $script:FixedTs = Parse-Timestamp $Timestamp

    $config   = Load-Config
    $certName = if (-not [string]::IsNullOrWhiteSpace($Name)) { $Name } else { $config.Name }
    $paths    = Get-CertPaths $certName

    if ($Create) {
        Write-Host "--- UnderVolter Certificate Generation ---" -ForegroundColor Cyan
        New-CertSet $paths
        Save-Config $certName

        Write-Host ""
        Write-Host "  Updating src/UnderVolterCert.h..." -ForegroundColor Cyan
        & powershell.exe -ExecutionPolicy Bypass -File $EmbedScript -CertDir $CertDir
        if ($LASTEXITCODE -ne 0) { throw "embed-cert.ps1 failed." }

        Write-Host ""
        Write-Warn "  IMPORTANT: Rebuild required to embed the certificate into UnderVolter.efi."
        Write-Warn "  Run: .\build.ps1"
        exit 0
    }

    # Signing mode
    $target = if (-not [string]::IsNullOrWhiteSpace($TargetPath)) {
        [System.IO.Path]::GetFullPath($TargetPath)
    } else {
        $DefaultTarget
    }

    if (-not (Test-Path -LiteralPath $target)) {
        Write-Step "Nothing to sign - file not found: $target"
        exit 0
    }

    Write-Host "--- UnderVolter EFI Signing ---" -ForegroundColor Cyan

    # Auto-create cert set if missing
    $required = @($paths.PfxPath, $paths.PasswordPath)
    if ($required | Where-Object { -not (Test-Path -LiteralPath $_) }) {
        Write-Info "  Certificate set not found - creating..."
        New-CertSet $paths
        Save-Config $certName

        Write-Host "  Updating src/UnderVolterCert.h..." -ForegroundColor Cyan
        & powershell.exe -ExecutionPolicy Bypass -File $EmbedScript -CertDir $CertDir
        if ($LASTEXITCODE -ne 0) { throw "embed-cert.ps1 failed." }

        Write-Warn "  Certificate created. The current EFI was built without it."
        Write-Warn "  Run .\build.ps1 again to embed the certificate, then it will be signed."
        exit 0
    }

    Assert-CertSet $paths
    $signTool = Get-SignTool
    Write-Step "  SignTool: $signTool"
    Sign-Efi $paths $target $signTool
}
catch {
    Write-Fail $_.Exception.Message
    exit 1
}
