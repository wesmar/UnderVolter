# UnderVolter QEMU/OVMF Script
# i7-9750H Coffee Lake - family=6, model=158, stepping=10
# Uruchom: .\run-qemu.ps1

$vmDir    = "C:\vm\qemu"
$qemu     = "C:\PROGRA~1\qemu\qemu-system-x86_64w.exe"
$ovmfCode = "C:\PROGRA~1\qemu\share\edk2-x86_64-code.fd"
$ovmfVars = "$vmDir\ovmf-vars.fd"
$efiSrc   = "$PSScriptRoot\bin\UnderVolter.efi"
$iniSrc   = "$PSScriptRoot\bin\UnderVolter.ini"
$efiDisk  = $vmDir
$efiDest  = "$efiDisk\undervolter.efi"
$iniDest  = "$efiDisk\UnderVolter.ini"
$startupNsh = "$efiDisk\startup.nsh"

# Ustawienia obrazu/okna (dopasuj pod swoje DPI/monitor)
$gfxWidth  = 1920
$gfxHeight = 1080
$zoomToFit = "on"  # on = mniejsze okno i mniejsze literki przez skalowanie; off = 1:1

Write-Host "`n=== UnderVolter QEMU ===" -ForegroundColor Cyan

foreach ($f in @($qemu, $ovmfCode, $ovmfVars, $efiSrc)) {
    if (!(Test-Path $f)) { Write-Error "Brak pliku: $f"; exit 1 }
}

# Odswiez efi-disk jesli EFI nowszy lub brak startup.nsh
New-Item -ItemType Directory -Force -Path $efiDisk | Out-Null
if (!(Test-Path $efiDest) -or (Get-Item $efiSrc).LastWriteTime -gt (Get-Item $efiDest).LastWriteTime) {
    Write-Host "Kopiowanie UnderVolter.efi -> efi-disk..." -ForegroundColor Yellow
    Copy-Item $efiSrc $efiDest -Force
    if (Test-Path $iniSrc) { Copy-Item $iniSrc $iniDest -Force }
    Write-Host "  Gotowe." -ForegroundColor Green
}
# startup.nsh: EFI Shell uruchamia UnderVolter i czeka na Enter
if (!(Test-Path $startupNsh)) {
    @(
        '@echo -off',
        'fs0:\undervolter.efi',
        'echo .',
        'echo === Gotowe. Nacisnij Enter zeby zamknac QEMU ===',
        'pause'
    ) | Set-Content $startupNsh -Encoding ASCII
}
# Usun stary bootx64.efi jesli istnieje
Remove-Item "$efiDisk\EFI\BOOT\BOOTX64.EFI" -Force -ErrorAction SilentlyContinue

Write-Host "Uruchamianie QEMU (Coffee Lake i7-9750H)..." -ForegroundColor Green
Write-Host "Zamknij okno QEMU zeby zatrzymac.`n" -ForegroundColor Yellow

$errFile = "$vmDir\qemu-error.log"
Remove-Item $errFile -ErrorAction SilentlyContinue

& $qemu `
    -M q35 `
    -cpu "qemu64,family=6,model=158,stepping=10" `
    -smp 4 `
    -drive "if=pflash,format=raw,readonly=on,file=$ovmfCode" `
    -drive "if=pflash,format=raw,file=$ovmfVars" `
    -drive "file=fat:rw:$efiDisk,format=raw,if=virtio" `
    -display "gtk,zoom-to-fit=$zoomToFit,show-menubar=off,show-tabs=off,window-close=on" `
    -vga none -device "bochs-display,xres=$gfxWidth,yres=$gfxHeight" `
    -fw_cfg "name=opt/ovmf/X-Resolution,string=$gfxWidth" `
    -fw_cfg "name=opt/ovmf/Y-Resolution,string=$gfxHeight" `
    -m 512M `
    -net none `
    -name "UnderVolter" 2>$errFile

$exitCode = $LASTEXITCODE
$errContent = if (Test-Path $errFile) { Get-Content $errFile } else { @() }
if ($errContent.Count -gt 0) {
    Write-Host "`nQEMU - blad (kod $exitCode):" -ForegroundColor Red
    $errContent | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Read-Host "`nNacisnij Enter zeby zamknac"
}
