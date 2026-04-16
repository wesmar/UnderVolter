// PrintStats.c — Post-programming display: GOP pixel table of per-domain V/F
//                settings (DrawDomainTable), per-core topology summary
//                (PrintCoreInfo), and V/F point dump (PrintVFPoints).
//                All text output uses UiConsole; the pixel table has an ASCII
//                fallback for when no GOP framebuffer is available.
#include <PiPei.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include "UiConsole.h"
#include <Protocol/MpService.h>
#include "Platform.h"
#include "CpuData.h"

/*******************************************************************************
 * Globals
 ******************************************************************************/

extern EFI_SYSTEM_TABLE*        gST;
extern EFI_MP_SERVICES_PROTOCOL* gMpServices;
extern UINT8  gPrintVFPoints_PostProgram;
extern UINT8  gPrintPackageConfig;
extern UINT32 gBCLK_bsp;  // BCLK in kHz, set during platform discovery

/*******************************************************************************
 * Helpers — typed RGB color and GOP pixel utilities
 ******************************************************************************/

typedef struct { UINT8 r, g, b; } RGB3;

static VOID R(UINTN x, UINTN y, UINTN w, UINTN h, RGB3 c)
{
  UiGfxFillRectRgb(x, y, w, h, c.r, c.g, c.b);
}

// Draw ASCII string centered / left / right within a cell.
// align: 0 = left (+4px pad), 1 = center, 2 = right (+4px pad)
static VOID TC(UINTN cx, UINTN cy, UINTN cw, UINTN ch,
               CONST CHAR8* s, UINT8 align,
               RGB3 col, UINTN fw, UINTN fh)
{
  UINTN len = 0;
  while (s[len]) { len++; }

  UINTN tw = len * fw;
  UINTN tx, ty;

  ty = cy + (ch > fh ? (ch - fh) / 2 : 1);

  if (align == 0) {
    tx = cx + 4;
  } else if (align == 2) {
    tx = cx + (cw > tw + 4 ? cw - tw - 4 : 0);
  } else {
    tx = cx + (cw > tw ? (cw - tw) / 2 : 0);
  }

  UiGfxDrawAsciiAt(tx, ty, s, col.r, col.g, col.b);
}

/*******************************************************************************
 * Domain table — design constants
 ******************************************************************************/

// 6 columns: Domain | VR Addr | SVID | IccMax | Mode | Offset
#define NCOLS 6

// Column char widths (at font cell width 16px → pixels = kCW[c] * fw)
static const UINT8 kCW[NCOLS]   = { 30,  8,  6,  8, 15, 12 };
// 0=left  1=center  2=right
static const UINT8 kCAl[NCOLS]  = {  0,  1,  1,  1,  1,  2 };

static const CHAR8* kCHdr[NCOLS] = {
  "Voltage Domain",
  "VR Addr",
  "SVID",
  "IccMax",
  "Mode",
  "Offset (mV)"
};

// Max 29 chars to fit in the 30-char column (kCW[0]=30, 480px at 16px/char)
static const CHAR8* kDomName[MAX_DOMAINS] = {
  "P-Core  (Intel Architecture)",   // 28
  "GT Slice  (GPU Execution)",       // 25
  "Ring Bus  (Cache)",               // 17
  "GT Unslice  (Fixed Functions)",   // 29  ← longest
  "Uncore  (System Agent)",          // 22
  "E-Core  (Efficiency Core)",       // 25
};

// Color palette
static const RGB3 kBorder  = {  0, 110, 160 };  // steel-teal border
static const RGB3 kHdrBg   = {  6,  22,  60 };  // dark-navy header bg
static const RGB3 kHdrFg   = {195, 228, 255 };  // light-blue header text
static const RGB3 kHdrSep  = {  0, 140, 200 };  // header bottom separator (brighter)
static const RGB3 kRowOdd  = { 10,  10,  26 };  // alternating row A
static const RGB3 kRowEven = {  4,   4,  14 };  // alternating row B
static const RGB3 kDomFg   = { 70, 195, 255 };  // electric-blue domain name
static const RGB3 kValFg   = {140, 185, 200 };  // muted-cyan values
static const RGB3 kUndervC = { 55, 215,  95 };  // green  — undervolt (negative)
static const RGB3 kOvervC  = {220,  75,  55 };  // red    — overvolt  (positive)
static const RGB3 kZeroC   = { 65,  65,  85 };  // dim    — zero offset
static const RGB3 kOvrdC   = {255, 160,  20 };  // orange — override mode

/*******************************************************************************
 * DrawDomainTable — full GOP pixel table for one package
 ******************************************************************************/

// Render a 6-column table directly into the framebuffer at pixel position
// (tX, tY).  Each row represents one active voltage domain.
// fw/fh are the current font cell dimensions in pixels.
// Columns: Domain name, VR address, SVID, IccMax, VoltMode, Offset/Target.
// Offset column is colour-coded: green=undervolt, red=overvolt, orange=override.
static VOID DrawDomainTable(UINTN tX, UINTN tY,
                             UINTN fw, UINTN fh,
                             PACKAGE* pac)
{
  UINTN rowH = fh + 6;   // data row height  (3px pad top & bottom)
  UINTN hdrH = fh + 10;  // header row height (5px pad top & bottom)

  // Build column pixel widths and X positions
  UINTN colPx[NCOLS];
  UINTN colX[NCOLS + 1];
  UINTN totalW = 0;

  for (UINTN c = 0; c < NCOLS; c++) {
    colPx[c]  = (UINTN)kCW[c] * fw;
    totalW   += colPx[c];
  }
  totalW += NCOLS + 1;   // 1px separators: left + 5 inner + right

  colX[0] = tX + 1;      // 1px left border
  for (UINTN c = 1; c <= NCOLS; c++) {
    colX[c] = colX[c-1] + colPx[c-1] + 1;
  }

  // ── Top border + header background ──────────────────────────────────────
  R(tX, tY, totalW, 1, kBorder);
  R(tX, tY + 1, totalW, hdrH, kHdrBg);

  // Header text
  for (UINTN c = 0; c < NCOLS; c++) {
    TC(colX[c], tY + 1, colPx[c], hdrH, kCHdr[c], 1, kHdrFg, fw, fh);
  }

  // Header column separators
  for (UINTN c = 1; c < NCOLS; c++) {
    R(colX[c] - 1, tY, 1, hdrH + 1, kBorder);
  }

  // Header-data separator (2px, slightly brighter)
  UINTN sepY = tY + 1 + hdrH;
  R(tX, sepY, totalW, 2, kHdrSep);

  // ── Data rows ────────────────────────────────────────────────────────────
  UINTN rowY    = sepY + 2;
  UINTN rowIdx  = 0;
  CHAR8 buf[40];

  for (UINTN didx = 0; didx < MAX_DOMAINS; didx++) {
    if (!VoltageDomainExists((UINT8)didx)) {
      continue;
    }
    DOMAIN* dom = pac->planes + didx;
    if (dom->VRaddr == INVALID_VR_ADDR) {
      continue;
    }

    // Alternating row background
    RGB3 rowBg = (rowIdx & 1) ? kRowOdd : kRowEven;
    R(tX, rowY, totalW, rowH, rowBg);

    // Row bottom rule + column separators
    R(tX, rowY + rowH, totalW, 1, kBorder);
    for (UINTN c = 1; c < NCOLS; c++) {
      R(colX[c] - 1, rowY, 1, rowH, kBorder);
    }

    // Col 0 — Full domain name (electric blue)
    TC(colX[0], rowY, colPx[0], rowH,
       kDomName[didx < MAX_DOMAINS ? didx : 0],
       0, kDomFg, fw, fh);

    // Col 1 — VR Address
    AsciiSPrint(buf, sizeof(buf), "0x%02x", (UINTN)dom->VRaddr);
    TC(colX[1], rowY, colPx[1], rowH, buf, 1, kValFg, fw, fh);

    // Col 2 — SVID capability
    TC(colX[2], rowY, colPx[2], rowH,
       (dom->VRtype & 1) == 0 ? "YES" : "N/A",
       1, kValFg, fw, fh);

    // Col 3 — IccMax (stored in 1/4 A units)
    UINTN amps = dom->IccMax ? (UINTN)(dom->IccMax >> 2) : 0;
    AsciiSPrint(buf, sizeof(buf), "%u A", amps);
    TC(colX[3], rowY, colPx[3], rowH, buf, 1, kValFg, fw, fh);

    // Col 4 — Voltage mode
    BOOLEAN isOvrd = (BOOLEAN)(dom->VoltMode & 1);
    TC(colX[4], rowY, colPx[4], rowH,
       isOvrd ? "Override" : "Interpolative",
       1, isOvrd ? kOvrdC : kValFg, fw, fh);

    // Col 5 — Offset or target voltage with color coding
    RGB3 offCol;
    if (isOvrd) {
      AsciiSPrint(buf, sizeof(buf), "%u mV", (UINTN)dom->TargetVolts);
      offCol = kOvrdC;
    } else {
      INT16 ov = dom->OffsetVolts;
      if (ov < 0) {
        UINT16 absv = (UINT16)(-(INT32)ov);
        AsciiSPrint(buf, sizeof(buf), "-%u mV", (UINTN)absv);
        offCol = kUndervC;
      } else if (ov > 0) {
        AsciiSPrint(buf, sizeof(buf), "+%u mV", (UINTN)(UINT16)ov);
        offCol = kOvervC;
      } else {
        buf[0] = '0'; buf[1] = '\0';
        offCol = kZeroC;
      }
    }
    TC(colX[5], rowY, colPx[5], rowH, buf, 2, offCol, fw, fh);

    rowY += rowH + 1;
    rowIdx++;
  }

  // ── Bottom border + left/right verticals ────────────────────────────────
  R(tX, rowY, totalW, 2, kBorder);
  UINTN tableH = rowY + 2 - tY;
  R(tX,              tY, 1, tableH, kBorder);
  R(tX + totalW - 1, tY, 1, tableH, kBorder);
}

/*******************************************************************************
 * PrintCoreInfo
 ******************************************************************************/

// Dump a one-line summary of each discovered logical processor: APIC ID,
// physical-core flag, hybrid architecture flag, E-Core flag, and package index.
VOID PrintCoreInfo()
{
  for (UINTN cidx = 0; cidx < gNumCores; cidx++) {
    CPUCORE* core = (CPUCORE*)gCorePtrs[cidx];
    UiAsciiPrint("Core %u  apic:%u  phys:%u  hybrid:%u  ecore:%u  pkg:%u\n",
      cidx,
      core->ApicID,
      core->IsPhysical,
      core->CpuInfo.HybridArch,
      core->IsECore,
      core->PkgIdx);
  }
  UiAsciiPrint("\n");
}

/*******************************************************************************
 * PrintVFPoints
 ******************************************************************************/

// Print per-package, per-domain V/F point table to the console.
// Only prints domains that had individual V/F points programmed (Program_VF_Points==2)
// or when gPrintVFPoints_PostProgram is set.
// Frequency is derived from FusedRatio * gBCLK_bsp / 1000 (result in MHz).
// Voltage offset sign is preserved: green for negative (undervolt), red for positive.

static const CHAR8* kVFDomainLabel[MAX_DOMAINS] = {
  "P-Core (IA)",
  "GT Slice",
  "Ring Bus",
  "GT Unslice",
  "Uncore",
  "E-Core",
};

VOID PrintVFPoints(IN PLATFORM* psys)
{
  for (UINTN pidx = 0; pidx < psys->PkgCnt; pidx++) {
    PACKAGE* pac = psys->packages + pidx;

    UiSetAttribute(EFI_LIGHTCYAN);
    UiAsciiPrint(" Package #%u\n", pidx);

    for (UINTN didx = 0; didx < MAX_DOMAINS; didx++) {
      if (!VoltageDomainExists((UINT8)didx)) {
        continue;
      }
      if ((didx != IACORE) && (didx != RING) && (didx != ECORE)) {
        continue;
      }

      DOMAIN* dom = pac->planes + didx;

      if ((pac->Program_VF_Points[didx] == 2) || (gPrintVFPoints_PostProgram != 0)) {

        UiSetAttribute(EFI_CYAN);
        UiAsciiPrint("  Domain: %s", kVFDomainLabel[didx]);
        UiSetAttribute(EFI_DARKGRAY);
        UiAsciiPrint("  (%u V/F points)\n", dom->nVfPoints);

        for (UINTN vidx = 0; vidx < dom->nVfPoints; vidx++) {
          VF_POINT* vp = dom->vfPoint + vidx;

          UiSetAttribute(EFI_DARKGRAY);
          UiAsciiPrint("    VFP#%u  ", vidx);

          UiSetAttribute(EFI_WHITE);
          UiAsciiPrint("%4u MHz", vp->FusedRatio * gBCLK_bsp / 1000);

          if (vp->VOffset != 0) {
            UiSetAttribute(vp->VOffset < 0 ? EFI_LIGHTGREEN : EFI_LIGHTRED);
            if (vp->VOffset < 0) {
              UiAsciiPrint("  -%d mV", (UINTN)(UINT16)(-(INT32)vp->VOffset));
            } else {
              UiAsciiPrint("  +%d mV", (UINTN)(UINT16)vp->VOffset);
            }
          } else {
            UiSetAttribute(EFI_DARKGRAY);
            UiAsciiPrint("  0 mV");
          }
          UiAsciiPrint("\n");
        }
      }
    }
    UiAsciiPrint("\n");
  }
  UiSetAttribute(EFI_WHITE);
}

/*******************************************************************************
 * PrintPlatformSettings
 ******************************************************************************/

// Print the per-package domain table for the full platform.
// When GOP is available and the screen is wide enough, renders the pixel table
// via DrawDomainTable and repositions the text cursor past it.
// If the table would overflow the screen (e.g. QEMU), scrolls up to make room.
// Falls back to a plain ASCII table when no framebuffer is present.
// Gated by gPrintPackageConfig; no-op when that flag is 0.
VOID PrintPlatformSettings(IN PLATFORM* psys)
{
  if (!gPrintPackageConfig) {
    return;
  }

  UINTN fw = 0, fh = 0;
  UiGfxGetCellSize(&fw, &fh);
  if (fw == 0) fw = 16;
  if (fh == 0) fh = 16;

  UINTN SW = 0, SH = 0;
  UiGfxGetDimensions(&SW, &SH);

  // Pre-compute table width (same for every package)
  UINTN tableW = 0;
  for (UINTN c = 0; c < NCOLS; c++) {
    tableW += (UINTN)kCW[c] * fw;
  }
  tableW += NCOLS + 1;   // separators

  for (UINTN pidx = 0; pidx < psys->PkgCnt; pidx++) {
    PACKAGE* pac = psys->packages + pidx;

    // ── Package heading ────────────────────────────────────────────────────
    UiPrint(L"\n");
    UiSetAttribute(EFI_LIGHTCYAN);
    UiPrint(L" Package %u  \x2014  ", pidx);   // U+2014 em dash
    UiSetAttribute(EFI_WHITE);
    UiAsciiPrint((CHAR8*)pac->CpuInfo.venString);
    UiPrint(L"\n\n");

    if (UiGfxIsReady() && SW > 0 && tableW <= SW) {
      // Center the table horizontally
      UINTN tX = (SW - tableW) / 2;

      // Pre-compute table pixel height so we can check if it fits
      UINTN rowH    = fh + 6;
      UINTN hdrH    = fh + 10;
      UINTN rowCount = 0;
      for (UINTN d = 0; d < MAX_DOMAINS; d++) {
        if (VoltageDomainExists((UINT8)d) && pac->planes[d].VRaddr != INVALID_VR_ADDR) {
          rowCount++;
        }
      }
      UINTN tablePx  = 1 + hdrH + 2 + rowCount * (rowH + 1) + 2;
      UINTN skipRows = (tablePx + fh - 1) / fh + 1;  // rows to skip past table + gap

      // Current cursor position
      UINTN curCol = 0, curRow = 0;
      UiGfxGetCursor(&curCol, &curRow);

      // If table overflows the screen, clear lower area and reposition cursor
      // so the full table is always visible (important on small screens like QEMU).
      UINTN totalRows = (fh > 0 && SH > 0) ? SH / fh : 40;
      if (curRow + skipRows + 1 > totalRows) {
        UINTN newRow = (totalRows > skipRows + 2) ? totalRows - skipRows - 2 : 0;
        UINTN clearY = newRow * fh;
        if (clearY < SH) {
          UiGfxFillRectRgb(0, clearY, SW, SH - clearY, 0, 0, 0);
        }
        curRow = newRow;
        UiGfxSetCursor(0, curRow);
      }

      UINTN tY = curRow * fh;

      DrawDomainTable(tX, tY, fw, fh, pac);

      UiGfxSetCursor(0, curRow + skipRows);

    } else {
      // ── ASCII fallback ─────────────────────────────────────────────────
      UiPrint(
        L"+----------------------------+--------+------+--------+---------------+-----------+\n"
        L"| Voltage Domain             | VR Addr| SVID | IccMax | Mode          | Offset    |\n"
        L"+----------------------------+--------+------+--------+---------------+-----------+\n"
      );

      for (UINTN didx = 0; didx < MAX_DOMAINS; didx++) {
        if (!VoltageDomainExists((UINT8)didx)) {
          continue;
        }
        DOMAIN* dom = pac->planes + didx;
        if (dom->VRaddr == INVALID_VR_ADDR) {
          continue;
        }

        CHAR8 offsetBuf[16];
        if (dom->VoltMode & 1) {
          AsciiSPrint(offsetBuf, sizeof(offsetBuf), "%u mV", (UINTN)dom->TargetVolts);
        } else if (dom->OffsetVolts < 0) {
          UINT16 av = (UINT16)(-(INT32)dom->OffsetVolts);
          AsciiSPrint(offsetBuf, sizeof(offsetBuf), "-%u mV", (UINTN)av);
        } else {
          AsciiSPrint(offsetBuf, sizeof(offsetBuf), "%d mV", (UINTN)(UINT16)dom->OffsetVolts);
        }

        UiAsciiPrint("| %-26s | 0x%02x   |  %s  | %4u A  | %-13s | %-9s |\n",
          kDomName[didx < MAX_DOMAINS ? didx : 0],
          (UINTN)dom->VRaddr,
          (dom->VRtype & 1) == 0 ? "YES" : "N/A",
          dom->IccMax >> 2,
          (dom->VoltMode & 1) ? "Override" : "Interpolative",
          offsetBuf);
      }
      UiPrint(
        L"+----------------------------+--------+------+--------+---------------+-----------+\n\n"
      );
    }
  }

  UiSetAttribute(EFI_WHITE);
}
