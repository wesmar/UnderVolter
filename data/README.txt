# UnderVolter - Release 04.2026

## PASSWORD: github.com
### Extract downloaded archive with password: github.com

---

## WHAT'S INSIDE

UnderVolter-latest/
|
+-- UnderVolter.efi  [124K]  Main UEFI application (voltage/power MSR writes)
+-- UnderVolter.ini  [20K]  Per-CPU configuration (Intel 2nd-15th gen, auto-selected by CPUID)
+-- README.txt                   This guide
|
+-- other-tools/                 Supporting Tools (OPTIONAL)
    |
    +-- IFRExtractor.exe  [536K]  IFR/HII BIOS disassembler (CLI + GUI, C++17)
    |                                  Extracts BIOS form structure and VarStore offsets
    |                                  Use to find CFG Lock / OC Lock byte offsets for UnderVolter.ini
    |
    +-- Loader.efi        [36K]  Chainloading UEFI loader (Mode A deployment)
    |                                  Copy as \EFI\BOOT\BOOTX64.EFI — runs UnderVolter.efi
    |                                  then chainloads the original boot sequence automatically
    |
    +-- UEFITool.exe      [15M]  UEFITool NE Alpha 72 — UEFI firmware image parser
    |                                  By Nikolaj Schlej / CodeRush + Vitaly Cheptsov / vit9696
    |                                  Use to extract IFR data from firmware images
    |
    +-- phoenixtool273/             PhoenixTool 2.73 — BIOS disassembly tool by Andy
        +-- PhoenixTool.exe  [2.1M]
        +-- [support files]

---

## QUICK START

### Windows — persistent installation (recommended):

  1. Mount the EFI System Partition (ESP) from elevated Command Prompt:
       mountvol X: /S
     (X: is an example — use any free drive letter)

  2. Copy files to the ESP:
       xcopy /Y UnderVolter.efi X:\EFI\BOOT
       xcopy /Y UnderVolter.ini X:\EFI\BOOT

  3. Add a UEFI boot entry pointing to \EFI\BOOT\UnderVolter.efi
     via BIOS Setup (Boot > Add Boot Option).
     UnderVolter will run before Windows on every boot.
     See the animated demo at https://github.com/wesmar/UnderVolter

  Alternatively — replace BOOTX64.EFI directly:
       copy /Y UnderVolter.efi X:\EFI\BOOT\BOOTX64.EFI
     Note: this replaces the default fallback boot file.
     Do NOT do this if Secure Boot is enabled — leave BOOTX64.EFI alone
     and use a named UEFI boot entry instead.

  4. Unmount: mountvol X: /D

### First run / testing — EFI Shell:
  1. Copy UnderVolter.efi and UnderVolter.ini to a FAT32 USB drive
  2. Boot from EFI Shell (BIOS > Boot Override > EFI Shell)
  3. Run: UnderVolter.efi
  4. Press ESC within 2 seconds to abort if unstable

### OpenCore:
  Add UnderVolter.efi as a Driver or Tool entry in config.plist.

---

## UnderVolter.efi [124K]

Native UEFI application for Intel CPU power management programming.
Runs directly from firmware before any operating system loads.

Supported Intel generations: 2nd (Sandy Bridge) through 15th (Arrow Lake)
CPUID-based auto-selection of voltage domain layout and MSR addresses.

Features:
  - Voltage offset programming (P-Core, E-Core, Ring, Uncore, GT)
  - Power limit configuration (PL1/PL2/PL3/PL4/PP0)
  - Turbo ratio control
  - V/F curve overrides
  - ICC Max configuration per domain
  - CFG Lock + OC Lock bypass via NVRAM Setup variable patching [SetupVar]
  - Secure Boot SelfEnroll — embedded root CA, no external tools [SecureBoot]
  - Emergency exit: 2-second ESC key window on startup

---

## UnderVolter.ini [20K]

Configuration file — edit before deploying.
Located next to UnderVolter.efi (same directory on ESP or USB).

Key sections:
  [General]        Enable = 1, TimeoutMs = 2000, LogLevel
  [VoltageOffsets] Core = -80, Ring = -50, Uncore = -50, GT = 0, ...
  [PowerLimits]    PL1 = 45000, PL2 = 65000, Enabled = 1, ...
  [TurboRatios]    MaxRatio = 0 (0 = do not override)
  [SetupVar]       NvramPatchEnabled = 0, Offset_0 = "0x3E:0x00", ...
  [SecureBoot]     SelfEnroll = 0, TryDeployedMode = 0, BootToFirmwareUI = 0

Per-CPU architecture profiles are auto-selected by CPUID at runtime.
Safe defaults are pre-configured — only adjust if you know your CPU's limits.

---

## IFRExtractor.exe [536K] - BIOS IFR/HII Disassembler

Dual-mode tool (CLI and GUI) for extracting Internal Forms Representation
from UEFI/BIOS firmware images and HII database exports.

Use to find CFG Lock, OC Lock, and other hidden Setup variable byte offsets
for use in the [SetupVar] section of UnderVolter.ini.

Workflow:
  1. Dump BIOS image (e.g. via flashrom or manufacturer utility)
  2. Extract IFR section with UEFITool or PhoenixTool
  3. Run IFRExtractor.exe on the extracted HII binary
  4. Search output for "CFG Lock" or "Overclocking Lock" — note the VarOffset
  5. Set that offset in UnderVolter.ini: Offset_0 = "0xXX:0x00"

CLI: IFRExtractor.exe <input.bin> [output.txt]
GUI: IFRExtractor.exe  (no arguments — opens GUI)

---

## Loader.efi [36K] - Optional Chainloading UEFI Loader

Loader.efi is completely optional. Most users do not need it.

Use it only if you want to deploy UnderVolter by replacing BOOTX64.EFI
AND still need to chainload the original boot sequence automatically.
Placed as \EFI\BOOT\BOOTX64.EFI, it finds UnderVolter.efi in the same
directory, runs it, then continues the normal BootOrder chain.

Do NOT use Loader.efi if Secure Boot is enabled — leave BOOTX64.EFI
untouched and add UnderVolter.efi as a separate named UEFI boot entry
from BIOS Setup instead (see Quick Start above).

---

## UEFITool NE Alpha 72 [15M]

UEFI firmware image parser and editor.
By Nikolaj Schlej (CodeRush) and Vitaly Cheptsov (vit9696).

Use to open BIOS dump images and extract IFR/HII sections for IFRExtractor.

---

## PhoenixTool 2.73 by Andy

BIOS disassembly tool for Phoenix/AMI/Insyde firmware images.
Extracts modules and IFR data from BIOS ROM files.

Usage: PhoenixTool.exe <bios.rom>

---

## CONTACT & SUPPORT

  GitHub:   https://github.com/wesmar/UnderVolter
  Email:    marek@wesolowski.eu.org

---

## LEGAL DISCLAIMER

This software is provided for educational and research purposes only.
Users are responsible for compliance with applicable laws.
CPU undervolting may void warranty and can cause system instability.
The author assumes no liability for misuse or hardware damage.

---

Release: 04.2026
(c) WESMAR 2026
