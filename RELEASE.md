# UnderVolter — September 2026

Native UEFI undervolting and power-limit utility for Intel CPUs.
Everything below is measured from this build, not carried over from the previous release.

## Build

| | |
|---|---|
| Date | 2026-09-07 |
| `UnderVolter.efi` | 121,696 bytes (118.8 KiB) |
| `Loader.efi` | 14,848 bytes |
| `UnderVolter.ini` | 19,331 bytes |
| SHA-256 of `UnderVolter.efi` | `c202dc61831c82e7b949c689d48adad0cfb10f66087233e008c6122e17e5cc1a` |
| Warning flags | `/W4 /WX` |
| Deterministic build | `/Brepro /experimental:deterministic` |

## CPU coverage

- 102 CPUID entries across 74 microarchitectures in the detection table.
- 44 of them carry a VR topology template, i.e. support voltage programming.
- 14 tuning profiles ship in `UnderVolter.ini`.

## Validation

- Release/x64 builds clean with `/W4 /WX`.
- QEMU/OVMF regression suite: **58 checks, 0 failures**.
- Authenticode digest, CMS signature, embedded root relationship and certificate chain verified before packaging.

## Shipped configuration

The bundled `UnderVolter.ini` is the author's Dell XPS 15 7590 setup.

- Setup variable: `Setup`, GUID `EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9`, layout size `0x17FD`.
- Setup patches applied:
  - `0x6ED : 0x00`
  - `0x789 : 0x00`
- `Profile.CoffeeLake` voltage offsets:
  - `OffsetVolts_IACORE = -180` mV
  - `OffsetVolts_RING = -100` mV
  - `OffsetVolts_UNCORE = 0` mV
  - `OffsetVolts_GTSLICE = -40` mV
  - `OffsetVolts_GTUNSLICE = -40` mV

## Changes in this release

- Loader: skip partition-only self entries and fall back to bootmgfw.efi (#2)

## Download

Download [`UnderVolter.7z`](https://github.com/wesmar/UnderVolter/releases/download/latest/UnderVolter.7z) and extract it with password `github.com`.
