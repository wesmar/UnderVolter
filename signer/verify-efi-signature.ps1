[CmdletBinding()]
param(
    [string]$TargetPath,
    [string]$HeaderPath
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

$ScriptRoot  = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$ProjectRoot = Split-Path -Parent $ScriptRoot

if (-not $TargetPath) { $TargetPath = Join-Path $ProjectRoot "bin\UnderVolter.efi" }
if (-not $HeaderPath) { $HeaderPath = Join-Path $ProjectRoot "src\UnderVolterCert.h" }

$TargetPath = [System.IO.Path]::GetFullPath($TargetPath)
$HeaderPath = [System.IO.Path]::GetFullPath($HeaderPath)

Add-Type -AssemblyName System.Security

function Write-Info([string]$m)    { Write-Host $m -ForegroundColor Cyan }
function Write-Step([string]$m)    { Write-Host $m -ForegroundColor DarkGray }
function Write-Success([string]$m) { Write-Host $m -ForegroundColor Green }
function Write-Warn([string]$m)    { Write-Host $m -ForegroundColor Yellow }
function Write-Fail([string]$m)    { Write-Host $m -ForegroundColor Red }

function Read-U16([byte[]]$Bytes, [int]$Offset) {
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Read-U32([byte[]]$Bytes, [int]$Offset) {
    return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Get-ByteSlice([byte[]]$Bytes, [int]$Offset, [int]$Length) {
    if ($Length -lt 0) {
        throw "Negative slice length requested."
    }
    [byte[]]$slice = New-Object byte[] $Length
    if ($Length -gt 0) {
        [Array]::Copy($Bytes, $Offset, $slice, 0, $Length)
    }
    return $slice
}

function Read-DerLength([byte[]]$Bytes, [ref]$Offset) {
    if ($Offset.Value -ge $Bytes.Length) {
        throw "Unexpected end of DER data while reading length."
    }

    $first = $Bytes[$Offset.Value]
    $Offset.Value++
    if ($first -lt 0x80) {
        return [int]$first
    }

    $lengthBytes = $first -band 0x7F
    if ($lengthBytes -eq 0) {
        throw "Indefinite DER lengths are not supported."
    }

    $length = 0
    for ($i = 0; $i -lt $lengthBytes; $i++) {
        if ($Offset.Value -ge $Bytes.Length) {
            throw "Unexpected end of DER data while reading long-form length."
        }
        $length = ($length -shl 8) -bor $Bytes[$Offset.Value]
        $Offset.Value++
    }
    return [int]$length
}

function Get-DerElement([byte[]]$Bytes, [int]$Offset, [byte]$ExpectedTag) {
    if ($Offset -ge $Bytes.Length) {
        throw "Unexpected end of DER data."
    }

    $start = $Offset
    $tag = $Bytes[$Offset]
    if ($tag -ne $ExpectedTag) {
        throw ("Unexpected DER tag 0x{0:X2}; expected 0x{1:X2}" -f $tag, $ExpectedTag)
    }

    $cursor = $Offset + 1
    $cursorRef = [ref]$cursor
    $contentLength = Read-DerLength -Bytes $Bytes -Offset $cursorRef
    $contentOffset = $cursorRef.Value
    $headerLength = $contentOffset - $start
    $totalLength = $headerLength + $contentLength

    if (($start + $totalLength) -gt $Bytes.Length) {
        throw "DER element extends past the end of the buffer."
    }

    return [ordered]@{
        Tag           = $tag
        Start         = $start
        HeaderLength  = $headerLength
        ContentOffset = $contentOffset
        ContentLength = $contentLength
        TotalLength   = $totalLength
        Encoded       = (Get-ByteSlice -Bytes $Bytes -Offset $start -Length $totalLength)
        Content       = (Get-ByteSlice -Bytes $Bytes -Offset $contentOffset -Length $contentLength)
    }
}

function Decode-DerOid([byte[]]$OidBytes) {
    if ($OidBytes.Length -eq 0) {
        throw "OID is empty."
    }

    $parts = New-Object System.Collections.Generic.List[string]
    $first = [int]$OidBytes[0]
    [void]$parts.Add([string][Math]::Floor($first / 40))
    [void]$parts.Add([string]($first % 40))

    $value = 0L
    for ($i = 1; $i -lt $OidBytes.Length; $i++) {
        $value = ($value -shl 7) -bor ($OidBytes[$i] -band 0x7F)
        if (($OidBytes[$i] -band 0x80) -eq 0) {
            [void]$parts.Add([string]$value)
            $value = 0L
        }
    }

    if ($value -ne 0) {
        throw "OID ended mid-component."
    }

    return ($parts -join ".")
}

function Get-HeaderCertificateBytes([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Header file not found: $Path"
    }

    $headerText = Get-Content -LiteralPath $Path -Raw
    if ($headerText -match 'UV_CERT_DER_SIZE\s+\(\(UINTN\)0\)') {
        throw "Embedded certificate is disabled in $Path (UV_CERT_DER_SIZE == 0)."
    }

    $matches = [regex]::Matches($headerText, '0x([0-9A-Fa-f]{2})')
    if ($matches.Count -eq 0) {
        throw "Could not find gUVCertDer bytes in $Path"
    }

    [byte[]]$bytes = for ($i = 0; $i -lt $matches.Count; $i++) {
        [Convert]::ToByte($matches[$i].Groups[1].Value, 16)
    }
    return $bytes
}

function Get-PeSignatureBlob([string]$Path) {
    [byte[]]$fileBytes = [System.IO.File]::ReadAllBytes($Path)
    if ($fileBytes.Length -lt 0x100) {
        throw "File is too small to be a PE image: $Path"
    }

    $e_lfanew = Read-U32 $fileBytes 0x3C
    if (($e_lfanew + 4) -ge $fileBytes.Length) {
        throw "Invalid PE header offset in $Path"
    }

    $peSig = [System.Text.Encoding]::ASCII.GetString($fileBytes, [int]$e_lfanew, 4)
    if ($peSig -ne ("PE" + [char]0 + [char]0)) {
        throw "Not a PE/COFF image: $Path"
    }

    $coffHeaderOffset = $e_lfanew + 4
    $optionalHeaderOffset = $coffHeaderOffset + 20
    $magic = Read-U16 $fileBytes $optionalHeaderOffset

    $dataDirStart = switch ($magic) {
        0x10B { $optionalHeaderOffset + 96 }
        0x20B { $optionalHeaderOffset + 112 }
        default { throw ("Unsupported PE optional header magic: 0x{0:X}" -f $magic) }
    }

    $checksumOffset = $optionalHeaderOffset + 64
    $certDirOffset = $dataDirStart + (8 * 4)
    $certOffset = Read-U32 $fileBytes $certDirOffset
    $certSize = Read-U32 $fileBytes ($certDirOffset + 4)

    if ($certOffset -eq 0 -or $certSize -eq 0) {
        throw "PE image has no Authenticode certificate table: $Path"
    }
    if (($certOffset + $certSize) -gt $fileBytes.Length) {
        throw "PE certificate table points outside the file: $Path"
    }

    $winCertLength = Read-U32 $fileBytes $certOffset
    $winCertRevision = Read-U16 $fileBytes ($certOffset + 4)
    $winCertType = Read-U16 $fileBytes ($certOffset + 6)
    if ($winCertLength -lt 8) {
        throw "WIN_CERTIFICATE length is invalid in $Path"
    }

    [byte[]]$pkcs7Bytes = $fileBytes[($certOffset + 8)..($certOffset + $winCertLength - 1)]

    return [ordered]@{
        FileBytes         = $fileBytes
        OptionalMagic     = $magic
        ChecksumOffset    = $checksumOffset
        CertDirOffset     = $certDirOffset
        CertTableOffset   = $certOffset
        CertTableSize     = $certSize
        WinCertLength     = $winCertLength
        WinCertRevision   = $winCertRevision
        WinCertType       = $winCertType
        Pkcs7Bytes        = $pkcs7Bytes
    }
}

function Get-SpcIndirectDigest([byte[]]$ContentBytes) {
    $outer = Get-DerElement -Bytes $ContentBytes -Offset 0 -ExpectedTag 0x30
    $cursor = $outer.ContentOffset
    $contentEnd = $outer.ContentOffset + $outer.ContentLength

    $data = Get-DerElement -Bytes $ContentBytes -Offset $cursor -ExpectedTag 0x30
    $cursor += $data.TotalLength

    $digestInfo = Get-DerElement -Bytes $ContentBytes -Offset $cursor -ExpectedTag 0x30
    $cursor += $digestInfo.TotalLength
    if ($cursor -ne $contentEnd) {
        throw "Unexpected trailing data in SpcIndirectDataContent."
    }

    $digestCursor = $digestInfo.ContentOffset
    $alg = Get-DerElement -Bytes $ContentBytes -Offset $digestCursor -ExpectedTag 0x30
    $digestCursor += $alg.TotalLength

    $algCursor = $alg.ContentOffset
    $oidElement = Get-DerElement -Bytes $ContentBytes -Offset $algCursor -ExpectedTag 0x06
    $oid = Decode-DerOid -OidBytes $oidElement.Content
    $algCursor += $oidElement.TotalLength

    if ($algCursor -lt ($alg.ContentOffset + $alg.ContentLength)) {
        $null = Get-DerElement -Bytes $ContentBytes -Offset $algCursor -ExpectedTag 0x05
    }

    $digestElement = Get-DerElement -Bytes $ContentBytes -Offset $digestCursor -ExpectedTag 0x04
    [byte[]]$digest = $digestElement.Content

    return [ordered]@{
        AlgorithmOid = $oid
        Digest       = $digest
    }
}

function Get-DerContentOffset([byte[]]$EncodedValue) {
    if ($EncodedValue.Length -lt 2) {
        throw "DER value is too short."
    }
    if ($EncodedValue[1] -lt 0x80) {
        return 2
    }
    $lengthBytes = $EncodedValue[1] -band 0x7F
    return 2 + $lengthBytes
}

function Get-DerBitStringPayload([byte[]]$EncodedBitString) {
    if ($EncodedBitString.Length -lt 4 -or $EncodedBitString[0] -ne 0x03) {
        throw "Value is not a DER BIT STRING."
    }

    $contentOffset = Get-DerContentOffset $EncodedBitString
    $unusedBits = $EncodedBitString[$contentOffset]
    if ($unusedBits -ne 0) {
        throw "Unsupported BIT STRING with $unusedBits unused bits."
    }

    [byte[]]$payload = $EncodedBitString[($contentOffset + 1)..($EncodedBitString.Length - 1)]
    return $payload
}

function Test-CertificateSignature([System.Security.Cryptography.X509Certificates.X509Certificate2]$IssuerCert,
                                   [System.Security.Cryptography.X509Certificates.X509Certificate2]$SubjectCert) {
    $certSeq = Get-DerElement -Bytes $SubjectCert.RawData -Offset 0 -ExpectedTag 0x30
    $cursor = $certSeq.ContentOffset

    $tbsCertificate = Get-DerElement -Bytes $SubjectCert.RawData -Offset $cursor -ExpectedTag 0x30
    $cursor += $tbsCertificate.TotalLength

    $signatureAlgorithm = Get-DerElement -Bytes $SubjectCert.RawData -Offset $cursor -ExpectedTag 0x30
    $cursor += $signatureAlgorithm.TotalLength

    $signatureAlgorithmOidElement = Get-DerElement -Bytes $SubjectCert.RawData -Offset $signatureAlgorithm.ContentOffset -ExpectedTag 0x06
    $signatureAlgorithmOid = Decode-DerOid -OidBytes $signatureAlgorithmOidElement.Content

    $signatureBitString = Get-DerElement -Bytes $SubjectCert.RawData -Offset $cursor -ExpectedTag 0x03
    [byte[]]$signatureBytes = Get-DerBitStringPayload -EncodedBitString $signatureBitString.Encoded

    $hashName = switch ($signatureAlgorithmOid) {
        "1.2.840.113549.1.1.11" { [System.Security.Cryptography.HashAlgorithmName]::SHA256 }
        "1.2.840.113549.1.1.5"  { [System.Security.Cryptography.HashAlgorithmName]::SHA1 }
        default { throw "Unsupported certificate signature algorithm OID: $signatureAlgorithmOid" }
    }

    $issuerRsa = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($IssuerCert)
    if (-not $issuerRsa) {
        throw "Issuer certificate does not expose an RSA public key."
    }

    return $issuerRsa.VerifyData(
        [byte[]]$tbsCertificate.Encoded,
        [byte[]]$signatureBytes,
        $hashName,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
}

function Get-AuthenticodeDigest([hashtable]$PeInfo, [string]$DigestOid) {
    $hashName = switch ($DigestOid) {
        "2.16.840.1.101.3.4.2.1" { "SHA256" }
        "1.3.14.3.2.26"          { "SHA1" }
        default { throw "Unsupported Authenticode digest OID: $DigestOid" }
    }

    $hasher = [System.Security.Cryptography.IncrementalHash]::CreateHash($hashName)
    $segments = @(
        @{ Start = 0; End = $PeInfo.ChecksumOffset },
        @{ Start = $PeInfo.ChecksumOffset + 4; End = $PeInfo.CertDirOffset },
        @{ Start = $PeInfo.CertDirOffset + 8; End = $PeInfo.CertTableOffset },
        @{ Start = $PeInfo.CertTableOffset + $PeInfo.CertTableSize; End = $PeInfo.FileBytes.Length }
    )

    foreach ($segment in $segments) {
        if ($segment.End -gt $segment.Start) {
            $hasher.AppendData([byte[]]$PeInfo.FileBytes, [int]$segment.Start, [int]($segment.End - $segment.Start))
        }
    }

    return $hasher.GetHashAndReset()
}

Write-Host "--- UnderVolter EFI Signature Verification ---" -ForegroundColor Cyan
Write-Host ""

Write-Step "Target: $TargetPath"
Write-Step "Header: $HeaderPath"
Write-Host ""

$rootBytes = Get-HeaderCertificateBytes -Path $HeaderPath
$embeddedRoot = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([byte[]]$rootBytes)

Write-Info "Embedded root certificate:"
$embeddedRoot | Format-List Subject, Issuer, Thumbprint, SerialNumber, NotBefore, NotAfter

$signature = Get-AuthenticodeSignature -FilePath $TargetPath
if (-not $signature.SignerCertificate) {
    throw "No Authenticode signer certificate found in $TargetPath (status: $($signature.Status))."
}

Write-Info "Explorer / WinVerifyTrust view on this PC:"
$signature | Format-List Status, StatusMessage, SignatureType, Path
$signature.SignerCertificate | Format-List Subject, Issuer, Thumbprint, SerialNumber, NotBefore, NotAfter

$peInfo = Get-PeSignatureBlob -Path $TargetPath
$cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
$cms.Decode([byte[]]$peInfo.Pkcs7Bytes)

if ($cms.SignerInfos.Count -ne 1) {
    Write-Warn "CMS contains $($cms.SignerInfos.Count) signer infos; the script will inspect the first one."
}

$cmsSigner = $cms.SignerInfos[0].Certificate
if (-not $cmsSigner) {
    throw "CMS signer certificate is missing from the EFI signature."
}

$cmsEmbeddedLeafCount = $cms.Certificates.Count
$cmsContainsRoot = @($cms.Certificates | Where-Object { $_.Thumbprint -eq $embeddedRoot.Thumbprint }).Count -gt 0
$cmsSignerMatchesWinTrust = $cmsSigner.Thumbprint -eq $signature.SignerCertificate.Thumbprint
$issuerMatchesEmbeddedRoot = $cmsSigner.Issuer -eq $embeddedRoot.Subject
$leafVerifiedByEmbeddedRoot = Test-CertificateSignature -IssuerCert $embeddedRoot -SubjectCert $cmsSigner

$spcDigest = Get-SpcIndirectDigest -ContentBytes $cms.ContentInfo.Content
$fileDigest = Get-AuthenticodeDigest -PeInfo $peInfo -DigestOid $spcDigest.AlgorithmOid
$digestMatches = [System.Linq.Enumerable]::SequenceEqual([byte[]]$spcDigest.Digest, [byte[]]$fileDigest)

$cmsSignatureOk = $false
try {
    $cms.CheckSignature($true)
    $cmsSignatureOk = $true
}
catch {
    $cmsSignatureOk = $false
}

$customChain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
$customChain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
$customChain.ChainPolicy.VerificationFlags = [System.Security.Cryptography.X509Certificates.X509VerificationFlags]::AllowUnknownCertificateAuthority
$customChain.ChainPolicy.ExtraStore.Add($embeddedRoot) | Out-Null
$customChainBuilt = $customChain.Build($cmsSigner)

Write-Info "Embedded CMS details:"
Write-Host ("  WIN_CERTIFICATE revision/type : 0x{0:X} / 0x{1:X}" -f $peInfo.WinCertRevision, $peInfo.WinCertType)
Write-Host ("  Certificates in CMS          : {0}" -f $cmsEmbeddedLeafCount)
Write-Host ("  Root cert embedded in CMS    : {0}" -f $cmsContainsRoot)
Write-Host ("  CMS signer thumbprint        : {0}" -f $cmsSigner.Thumbprint)
Write-Host ("  CMS signer == WinTrust signer: {0}" -f $cmsSignerMatchesWinTrust)
Write-Host ("  CMS signature verifies       : {0}" -f $cmsSignatureOk)
Write-Host ("  Authenticode digest OID      : {0}" -f $spcDigest.AlgorithmOid)
Write-Host ("  Authenticode digest match    : {0}" -f $digestMatches)
Write-Host ("  Signer issuer == header root : {0}" -f $issuerMatchesEmbeddedRoot)
Write-Host ("  Leaf cert verified by root   : {0}" -f $leafVerifiedByEmbeddedRoot)
Write-Host ("  Chain builds with header root: {0}" -f $customChainBuilt)

if ($customChain.ChainStatus.Count -gt 0) {
    Write-Host "  Chain status:"
    $customChain.ChainStatus | Format-Table -AutoSize Status, StatusInformation
}

$isTrustedByEmbeddedRoot = $cmsSignatureOk -and $digestMatches -and $issuerMatchesEmbeddedRoot -and $leafVerifiedByEmbeddedRoot

Write-Host ""
if ($issuerMatchesEmbeddedRoot -and $leafVerifiedByEmbeddedRoot) {
    Write-Success "Result 1: gUVCertDer is the issuer / trust anchor for the signer certificate in $TargetPath."
} else {
    Write-Fail "Result 1: gUVCertDer does NOT match the issuer of the signer certificate in $TargetPath."
}

if ($isTrustedByEmbeddedRoot) {
    Write-Success "Result 2: the EFI Authenticode signature is cryptographically valid, and its signer chains to the public key from gUVCertDer."
    Write-Warn "          The root public key does not sign the EFI directly; it verifies the leaf signing certificate, while the leaf certificate verifies the CMS signature on the file."
} else {
    Write-Fail "Result 2: the EFI signature does NOT validate end-to-end against the embedded root certificate."
}

if ($TargetPath -like "*\\x64\\Release\\UnderVolter.efi") {
    Write-Host ""
    Write-Warn "Note: build.ps1 signs bin\\UnderVolter.efi. If x64\\Release\\UnderVolter.efi is unsigned, Secure Boot will reject it."
}
