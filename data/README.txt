UnderVolter - September 2026
================================

Archive password: github.com

CONTENTS
--------

UnderVolter.efi
    Signed x64 UEFI application.

UnderVolter.ini
    Dell XPS 15 7590 configuration used by the author. The CFG Lock and OC Lock
    offsets remain 0x6ED and 0x789. The Coffee Lake voltage offsets remain
    -180/-100/0/-40/-40 mV.

README.txt
    This file.

other-tools\
    Optional firmware-analysis utilities and Loader.efi. UnderVolter does not
    require Loader.efi when it is started directly by firmware, OpenCore, or an
    EFI Shell.

QUICK START
-----------

1. Extract the archive with password: github.com
2. Review UnderVolter.ini before using it on any machine other than the exact
   XPS 15 7590 configuration for which its Setup-variable offsets were recorded.
3. Put UnderVolter.efi and UnderVolter.ini in the same directory on a FAT32 EFI
   System Partition or USB drive.
4. Start UnderVolter.efi from an EFI Shell, a firmware boot entry, or OpenCore.
5. Press ESC during the startup window to abort CPU programming.

The configuration file must be named UnderVolter.ini. Names such as
UnderVolter_XPS.ini are not discovered automatically.

FIRST BOOT ON THE XPS 15 7590
-----------------------------

The supplied INI enables two idempotent bootstrap stages:

1. NVRAM Setup patching writes 0 to offsets 0x6ED and 0x789 only when either
   byte differs, verifies the complete variable, and can perform one warm reset.
2. Secure Boot SelfEnroll verifies the embedded root CA in db/KEK/PK, fills
   missing entries when firmware permits it, and can perform one warm reset.
3. On the next pass, both completed stages are no-ops and CPU settings are
   applied normally.

If both stages are needed, seeing UnderVolter during two consecutive reboot
passes is expected. It does not install a duplicate boot entry. Existing
BootNext is preserved; if BootNext is absent, the current boot entry is used for
the intentional bootstrap restart.

SECURE BOOT
-----------

SelfEnroll installs the embedded root CA into db and KEK. It creates PK only
when PK is absent and does not replace an existing Platform Key. Firmware policy
must allow the writes. Every write is checked by reading the variable back.
Rejected or unchanged enrollment does not cause an enrollment reboot.

The release image is Authenticode-signed with the shared demonstration key in
the public source repository. This makes the signing and SelfEnroll workflow
reproducible, but the signature does not authenticate a unique publisher because
anyone can use that key. Use your own root and signing key when that distinction
matters. PFX and password files are never needed on the ESP.

SUPPORTED PROCESSORS
--------------------

Profiles cover Sandy Bridge through Arrow Lake where the required Intel MSR and
OC mailbox paths are available. Lunar Lake is detected, but voltage programming
is intentionally rejected because a supported hardware path has not been
confirmed.

VALIDATION
----------

The release workflow requires:

- a clean Release/x64 build with warning level 4 and warnings treated as errors;
- end-to-end verification of the EFI Authenticode signature against the root
  certificate embedded in the image;
- the QEMU/OVMF regression application to report 58 checks and zero failures;
- a production-image LoadImage/StartImage test with the supplied INI.

QEMU cannot validate real voltage stability, OEM MSR behavior, thermal margins,
or platform-specific NVRAM policy. Validate those on the target machine.

PROJECT
-------

https://github.com/wesmar/UnderVolter

Copyright (c) 2026 Marek Wesolowski (WESMAR)
