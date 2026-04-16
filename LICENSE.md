# MIT License

## UnderVolter — Native UEFI Undervolting Utility

**Copyright (c) 2026 Marek Wesołowski (WESMAR)**

---

## License Grant

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

## Attribution Requirement

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

## Disclaimer of Warranty

**THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.**

---

## Project Information

- **Project:** UnderVolter — Native UEFI Undervolting Utility
- **Author:** Marek Wesołowski (WESMAR)
- **Contact:** [marek@wesolowski.eu.org](mailto:marek@wesolowski.eu.org)
- **GitHub:** [https://github.com/wesmar/UnderVolter](https://github.com/wesmar/UnderVolter)

### Technical Specifications

- **Platform:** UEFI firmware (EFI application, bare-metal, no OS required)
- **Languages:** C (UEFI application core), Assembly x64 (MSR/MMIO access), C++17 with RAII (IFRExtractor), PowerShell (build and certificate signing)
- **Architecture:** Native UEFI application — direct MSR and MMIO access from firmware, before any OS loads
- **Purpose:** Intel CPU voltage offset, power limit, and turbo ratio programming via UEFI

---

## Intended Use

This software is provided for:

- **Personal use** — undervolting your own Intel CPU for power efficiency and thermal management
- **Research and education** — studying UEFI application development, MSR programming, and Secure Boot enrollment
- **Open source contribution** — extending CPU coverage, adding new features, improving documentation

Users are responsible for compliance with applicable laws and hardware warranty terms. CPU undervolting modifies hardware operating parameters — test conservatively and keep an emergency exit path available (ESC key window on startup).

---

## Contributing

Contributions are welcome. Guidelines:

- **Language:** C (UEFI core), C++17 with RAII (tools), Assembly (performance-critical MSR paths)
- **Style:** RAII resource management in C++ components; explicit ownership in C components
- **Testing:** Verify on real hardware where possible; QEMU for initial smoke tests
- **CPU coverage:** New architecture support requires validated MSR addresses and voltage domain layout
- **Attribution:** Contributors will be acknowledged in project documentation

---

## Third-Party Components

The `other-tools/` directory includes third-party tools distributed for convenience:

- **UEFITool NE Alpha 72** — by Nikolaj Schlej (CodeRush) and Vitaly Cheptsov (vit9696) — BSD-2-Clause
- **PhoenixTool 2.73** — by Andy — freeware, redistributed as-is
- **UEFI EDK2 libraries** (`lib/`) — TianoCore EDK2 — BSD-2-Clause

Each component retains its own license. UnderVolter's MIT license applies to the UnderVolter source code and IFRExtractor only.

---

## Hardware Safety Notice

UnderVolter writes directly to CPU Model-Specific Registers (MSRs) and MMIO power management interfaces. Incorrect voltage offsets can cause system instability or data loss. The software includes a 2-second ESC abort window and conservative safe defaults, but:

- Test with small voltage offsets first
- Keep recovery media available
- The author assumes no liability for hardware damage or data loss

---

*Copyright (c) 2026 Marek Wesołowski (WESMAR). All rights reserved under MIT License terms.*
