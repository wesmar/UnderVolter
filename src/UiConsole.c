// UiConsole.c — GOP framebuffer text/graphics console.  Renders SSFN2 glyphs
//               at configurable scale (2× on large displays, 1× on small),
//               handles CR/LF/tab/scroll, and exposes a pixel-drawing API used
//               by ConsoleUi.c and PrintStats.c.  Falls back to UEFI ConOut
//               when no linear framebuffer is available.
#include "UiConsole.h"

#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/GraphicsOutput.h>

#include "BmpFont.h"

extern EFI_SYSTEM_TABLE* gST;

// Scalable Screen Font (https://gitlab.com/bztsrc/scalable-font2)
typedef struct {
  unsigned char  magic[4];
  unsigned int   size;
  unsigned char  type;
  unsigned char  features;
  unsigned char  fbWidth;
  unsigned char  fbHeight;
  unsigned char  baseline;
  unsigned char  underline;
  unsigned short fragments_offs;
  unsigned int   characters_offs;
  unsigned int   ligature_offs;
  unsigned int   kerning_offs;
  unsigned int   cmap_offs;
} ssfn_font_t;

static ssfn_font_t* gFont = (ssfn_font_t*)&_bmp_font[0];

// Global text scaling (pixel sizes are derived from glyph metrics)
#define UI_TEXT_SCALE_LARGE 2
#define UI_TEXT_SCALE_SMALL 1

static EFI_GRAPHICS_OUTPUT_PROTOCOL* gGop = NULL;
static UINT32* gLfb = NULL;
static UINTN gFbWidth = 0;
static UINTN gFbHeight = 0;
static UINTN gPitchPixels = 0;
static UINTN gCellW = 0;
static UINTN gCellH = 0;
static UINTN gGlyphW = 0;
static UINTN gGlyphH = 0;
static UINTN gTextScaleX = UI_TEXT_SCALE_LARGE;
static UINTN gTextScaleY = UI_TEXT_SCALE_LARGE;
static UINTN gCols = 0;
static UINTN gRows = 0;
static UINTN gCursorX = 0;
static UINTN gCursorY = 0;
static BOOLEAN gGfxReady = FALSE;
static BOOLEAN gPendingCR = FALSE;
static BOOLEAN gPixelBgr = TRUE;
static UINT32 gFgColor = 0x00FFFFFF;
static UINT32 gBgColor = 0x00000000;
static UINT32 gColorLut[16] = { 0 };

// Pack an RGB triplet into the native 32-bit framebuffer pixel format.
// gPixelBgr=TRUE  → PixelBlueGreenRedReserved (most common on x64 firmware)
// gPixelBgr=FALSE → PixelRedGreenBlueReserved
static UINT32 MakeColor(UINT8 r, UINT8 g, UINT8 b)
{
  if (gPixelBgr) {
    return ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
  }

  // PixelRedGreenBlueReserved8BitPerColor
  return ((UINT32)b << 16) | ((UINT32)g << 8) | (UINT32)r;
}

static VOID InitColorLut(VOID)
{
  gColorLut[EFI_BLACK]        = MakeColor(0x00, 0x00, 0x00);
  gColorLut[EFI_BLUE]         = MakeColor(0x00, 0x00, 0xAA);
  gColorLut[EFI_GREEN]        = MakeColor(0x00, 0xAA, 0x00);
  gColorLut[EFI_CYAN]         = MakeColor(0x00, 0xAA, 0xAA);
  gColorLut[EFI_RED]          = MakeColor(0xAA, 0x00, 0x00);
  gColorLut[EFI_MAGENTA]      = MakeColor(0xAA, 0x00, 0xAA);
  gColorLut[EFI_BROWN]        = MakeColor(0xAA, 0x55, 0x00);
  gColorLut[EFI_LIGHTGRAY]    = MakeColor(0xAA, 0xAA, 0xAA);
  gColorLut[EFI_DARKGRAY]     = MakeColor(0x55, 0x55, 0x55);
  gColorLut[EFI_LIGHTBLUE]    = MakeColor(0x55, 0x55, 0xFF);
  gColorLut[EFI_LIGHTGREEN]   = MakeColor(0x55, 0xFF, 0x55);
  gColorLut[EFI_LIGHTCYAN]    = MakeColor(0x55, 0xFF, 0xFF);
  gColorLut[EFI_LIGHTRED]     = MakeColor(0xFF, 0x55, 0x55);
  gColorLut[EFI_LIGHTMAGENTA] = MakeColor(0xFF, 0x55, 0xFF);
  gColorLut[EFI_YELLOW]       = MakeColor(0xFF, 0xFF, 0x55);
  gColorLut[EFI_WHITE]        = MakeColor(0xFF, 0xFF, 0xFF);
}

// Walk the SSFN2 character table and return the maximum advance width among
// printable ASCII glyphs (codepoints 32..126).  Used to derive gCellW.
// Returns 8 if the font header is absent or all widths are zero.
static UINTN ComputeAsciiGlyphWidth(VOID)
{
  if (!gFont || gFont->characters_offs == 0 || gFont->size == 0) {
    return 8;
  }

  UINT8* ptr = (UINT8*)gFont + gFont->characters_offs;
  UINT8* end = (UINT8*)gFont + gFont->size;

  UINTN cp = 0;
  UINTN maxW = 0;

  while (cp < 0x110000 && ptr < end) {
    UINT8 c0 = ptr[0];

    if (c0 == 0xFF) {
      cp += 65536;
      ptr += 1;
      continue;
    } else if ((c0 & 0xC0) == 0xC0) {
      if (ptr + 1 >= end) {
        break;
      }
      UINTN j = (((UINTN)c0 & 0x3F) << 8) | ptr[1];
      cp += j;
      ptr += 2;
      continue;
    } else if ((c0 & 0xC0) == 0x80) {
      UINTN j = (c0 & 0x3F);
      cp += j;
      ptr += 1;
      continue;
    }

    if (cp >= 32 && cp <= 126) {
      UINTN w = ptr[4];
      if (w > maxW) {
        maxW = w;
      }
    }

    UINTN segs = ptr[1];
    UINTN step = 6 + segs * ((c0 & 0x40) ? 6 : 5);
    ptr += step;
    cp++;
  }

  if (maxW == 0) {
    maxW = (gFont && gFont->fbWidth) ? gFont->fbWidth : 8;
  }

  return maxW;
}

static VOID GfxFillRect(UINTN x, UINTN y, UINTN w, UINTN h, UINT32 color)
{
  if (!gGfxReady || !gLfb || w == 0 || h == 0) {
    return;
  }

  if (x >= gFbWidth || y >= gFbHeight) {
    return;
  }

  if (x + w > gFbWidth) {
    w = gFbWidth - x;
  }
  if (y + h > gFbHeight) {
    h = gFbHeight - y;
  }

  UINT32* row = gLfb + y * gPitchPixels + x;
  for (UINTN ry = 0; ry < h; ry++) {
    for (UINTN rx = 0; rx < w; rx++) {
      row[rx] = color;
    }
    row += gPitchPixels;
  }
}

static VOID GfxClearScreen(VOID)
{
  GfxFillRect(0, 0, gFbWidth, gFbHeight, gBgColor);
}

static VOID GfxClearLine(UINTN line)
{
  if (line >= gRows) {
    return;
  }
  GfxFillRect(0, line * gCellH, gFbWidth, gCellH, gBgColor);
}

// Scroll the framebuffer up by one text row (gCellH pixels) using CopyMem,
// then clear the newly exposed bottom row.
static VOID GfxScroll(VOID)
{
  if (gCellH == 0 || gFbHeight <= gCellH) {
    gCursorX = 0;
    gCursorY = 0;
    GfxClearScreen();
    return;
  }

  UINTN rowPixels = gCellH;
  UINTN copyPixels = (gFbHeight - rowPixels) * gPitchPixels;

  if (copyPixels > 0) {
    CopyMem(gLfb, gLfb + rowPixels * gPitchPixels, copyPixels * sizeof(UINT32));
  }

  GfxFillRect(0, gFbHeight - rowPixels, gFbWidth, rowPixels, gBgColor);

  if (gRows > 0) {
    gCursorY = gRows - 1;
  } else {
    gCursorY = 0;
  }
  gCursorX = 0;
}

static UINT32 FallbackGlyph(UINT32 codepoint);

// Locate the SSFN2 character descriptor for the given Unicode codepoint by
// walking the run-length-encoded character table.  Returns a pointer to the
// raw descriptor bytes, or NULL if not found.
static unsigned char* FindGlyph(UINT32 codepoint)
{
  unsigned char* ptr = (unsigned char*)gFont + gFont->characters_offs;
  unsigned char* chr = 0;

  for (UINT32 i = 0; i < 0x110000; i++) {
    if (ptr[0] == 0xFF) {
      i += 65535;
      ptr++;
    } else if ((ptr[0] & 0xC0) == 0xC0) {
      UINT32 j = (((ptr[0] & 0x3F) << 8) | ptr[1]);
      i += j;
      ptr += 2;
    } else if ((ptr[0] & 0xC0) == 0x80) {
      UINT32 j = (ptr[0] & 0x3F);
      i += j;
      ptr++;
    } else {
      if (i == codepoint) {
        chr = ptr;
        break;
      }
      ptr += 6 + ptr[1] * (ptr[0] & 0x40 ? 6 : 5);
    }
  }

  return chr;
}

static BOOLEAN DrawCopyrightGlyph(UINTN x, UINTN y, UINT32 color, UINTN scaleX, UINTN scaleY)
{
  // 8x8 bitmap for ©, centered inside the base glyph box
  static const UINT8 kCopyright8[8] = {
    0x3C, // 00111100
    0x42, // 01000010
    0x84, // 10000100
    0x80, // 10000000
    0x80, // 10000000
    0x84, // 10000100
    0x42, // 01000010
    0x3C  // 00111100
  };

  UINTN gw = gGlyphW ? gGlyphW : 8;
  UINTN gh = gGlyphH ? gGlyphH : 16;
  UINTN offX = (gw > 8) ? (gw - 8) / 2 : 0;
  UINTN offY = (gh > 8) ? (gh - 8) / 2 : 0;

  for (UINTN row = 0; row < 8; row++) {
    UINT8 bits = kCopyright8[row];
    for (UINTN col = 0; col < 8; col++) {
      if (bits & (0x80 >> col)) {
        GfxFillRect(x + (offX + col) * scaleX,
                    y + (offY + row) * scaleY,
                    scaleX, scaleY, color);
      }
    }
  }

  return TRUE;
}

// Render one SSFN2 glyph to the framebuffer with independent X/Y scale factors.
// Each set source pixel becomes a scaleX×scaleY filled rectangle.
// Falls back to the © bitmap stub for codepoint 0x00A9, then to FallbackGlyph.
static BOOLEAN GfxDrawGlyphScaledXY(UINT32 codepoint, UINTN x, UINTN y,
                                    UINT32 color, UINTN scaleX, UINTN scaleY)
{
  if (!gGfxReady || !gLfb || x >= gFbWidth || y >= gFbHeight) {
    return FALSE;
  }

  if (scaleX == 0) scaleX = 1;
  if (scaleY == 0) scaleY = 1;

  unsigned char* chr = FindGlyph(codepoint);
  if (!chr) {
    if (codepoint == 0x00A9) {
      return DrawCopyrightGlyph(x, y, color, scaleX, scaleY);
    }
    UINT32 fallback = FallbackGlyph(codepoint);
    if (fallback) {
      chr = FindGlyph(fallback);
    }
    if (!chr) {
      return FALSE;
    }
  }

  unsigned char* ptr = chr + 6;
  unsigned char* frg;
  int i, j, k, l, m, n;

  // Iterate over each fragment in the SSFN2 character descriptor.
  // chr[0] bit6=1 → 4-byte fragment offset (large font); bit6=0 → 3-byte offset.
  // Each fragment header: frg[0] bits 4:0 = column count / 8 - 1; frg[1] = row count - 1.
  // Bitmap follows frg[2]: 1 bit per pixel, MSB first, row-major.
  for (i = n = 0; i < chr[1]; i++, ptr += chr[0] & 0x40 ? 6 : 5) {
    if (ptr[0] == 255 && ptr[1] == 255) {
      continue;
    }

    frg = (unsigned char*)gFont + (chr[0] & 0x40 ?
      ((ptr[5] << 24) | (ptr[4] << 16) | (ptr[3] << 8) | ptr[2]) :
      ((ptr[4] << 16) | (ptr[3] << 8) | ptr[2]));

    if ((frg[0] & 0xE0) != 0x80) {
      continue;
    }

    n = (int)ptr[1];          // absolute glyph-row of this fragment
    k = ((frg[0] & 0x1F) + 1) << 3;  // bit-width (columns) per row
    j = (int)frg[1] + 1;       // row count of this fragment
    frg += 2;

    for (m = 1; j; j--, n++) {
      for (l = 0; l < k; l++, m <<= 1) {
        if (m > 0x80) { frg++; m = 1; }
        if (*frg & m) {
          GfxFillRect(x + (UINTN)l * scaleX,
                      y + (UINTN)n * scaleY,
                      scaleX, scaleY, color);
        }
      }
    }
  }

  return TRUE;
}

static BOOLEAN GfxDrawGlyph(UINT32 codepoint, UINTN x, UINTN y, UINT32 color)
{
  return GfxDrawGlyphScaledXY(codepoint, x, y, color, 1, 1);
}

// Map box-drawing characters and common Latin Extended-A letters to their
// nearest ASCII equivalents for fonts that lack those glyphs.
static UINT32 FallbackGlyph(UINT32 codepoint)
{
  switch (codepoint) {
    case 0x2554: // ╔
    case 0x2557: // ╗
    case 0x255A: // ╚
    case 0x255D: // ╝
    case 0x2566: // ╦
    case 0x2569: // ╩
    case 0x2560: // ╠
    case 0x2563: // ╣
    case 0x256C: // ╬
    case 0x253C: // ┼
    case 0x252C: // ┬
    case 0x2534: // ┴
    case 0x251C: // ├
    case 0x2524: // ┤
    case 0x250C: // ┌
    case 0x2510: // ┐
    case 0x2514: // └
    case 0x2518: // ┘
      return '+';
    case 0x2550: // ═
    case 0x2500: // ─
      return '-';
    case 0x2551: // ║
    case 0x2502: // │
      return '|';
    // Latin Extended-A — common accented/special letters not in basic VGA font
    case 0x0142: return 'l';  // ł
    case 0x0141: return 'L';  // Ł
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3:
    case 0x00E4: case 0x00E5: return 'a';  // àáâãäå
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
    case 0x00C4: case 0x00C5: return 'A';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return 'e';  // èéêë
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return 'E';
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return 'i';  // ìíîï
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return 'I';
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: return 'o';
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: return 'O';
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return 'u';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return 'U';
    case 0x00F1: return 'n';  // ñ
    case 0x00D1: return 'N';
    case 0x00E7: return 'c';  // ç
    case 0x00C7: return 'C';
    default:
      return 0;
  }
}

static VOID GfxPutChar(UINT32 codepoint)
{
  if (!gGfxReady) {
    return;
  }

  if (codepoint == '\r') {
    gCursorX = 0;
    gPendingCR = TRUE;
    return;
  }

  if (codepoint == '\n') {
    gCursorX = 0;
    gPendingCR = FALSE;
    gCursorY++;
    if (gCursorY >= gRows) {
      GfxScroll();
    }
    return;
  }

  if (codepoint == '\t') {
    UINTN spaces = 4 - (gCursorX % 4);
    for (UINTN i = 0; i < spaces; i++) {
      GfxPutChar(' ');
    }
    return;
  }

  if (codepoint < 0x20) {
    return;
  }

  if (gPendingCR) {
    GfxClearLine(gCursorY);
    gPendingCR = FALSE;
  }

  if (gCursorY >= gRows) {
    GfxScroll();
  }

  UINTN x = gCursorX * gCellW;
  UINTN y = gCursorY * gCellH;
  GfxFillRect(x, y, gCellW, gCellH, gBgColor);

  GfxDrawGlyphScaledXY(codepoint, x, y, gFgColor, gTextScaleX, gTextScaleY);

  gCursorX++;
  if (gCursorX >= gCols) {
    gCursorX = 0;
    gCursorY++;
    if (gCursorY >= gRows) {
      GfxScroll();
    }
  }
}

static VOID GfxWriteUnicode(IN CONST CHAR16* Str)
{
  if (!Str) {
    return;
  }
  while (*Str) {
    GfxPutChar((UINT32)(*Str));
    Str++;
  }
}

static VOID GfxWriteAscii(IN CONST CHAR8* Str)
{
  if (!Str) {
    return;
  }
  while (*Str) {
    GfxPutChar((UINT8)(*Str));
    Str++;
  }
}

// Locate the GOP protocol, map the linear framebuffer, compute glyph metrics,
// and select text scale (2× for ≥700-line screens, 1× otherwise).
// Must be called before any Ui* output function.  Returns TRUE on success.
BOOLEAN UiConsoleInit(IN EFI_SYSTEM_TABLE* SystemTable)
{
  if (gGfxReady || SystemTable == NULL) {
    return gGfxReady;
  }

  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
  EFI_STATUS status = SystemTable->BootServices->LocateProtocol(
    &gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&gop);

  if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
    return FALSE;
  }

  if (gop->Mode->Info->PixelFormat == PixelBltOnly) {
    return FALSE;
  }

  gGop = gop;
  gLfb = (UINT32*)(UINTN)gop->Mode->FrameBufferBase;
  gFbWidth = gop->Mode->Info->HorizontalResolution;
  gFbHeight = gop->Mode->Info->VerticalResolution;
  gPitchPixels = gop->Mode->Info->PixelsPerScanLine;

  gPixelBgr = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor);
  InitColorLut();

  gGlyphW = ComputeAsciiGlyphWidth();
  gGlyphH = (gFont && gFont->fbHeight) ? gFont->fbHeight : 16;
  if (gGlyphW == 0) gGlyphW = 8;
  if (gGlyphH == 0) gGlyphH = 16;

  if (gFbHeight < 700 || gFbWidth < 1024) {
    gTextScaleX = UI_TEXT_SCALE_SMALL;
    gTextScaleY = UI_TEXT_SCALE_SMALL;
  } else {
    gTextScaleX = UI_TEXT_SCALE_LARGE;
    gTextScaleY = UI_TEXT_SCALE_LARGE;
  }

  gCellW = gGlyphW * gTextScaleX;
  gCellH = gGlyphH * gTextScaleY;
  gCols = (gCellW > 0) ? (gFbWidth / gCellW) : 0;
  gRows = (gCellH > 0) ? (gFbHeight / gCellH) : 0;

  gCursorX = 0;
  gCursorY = 0;
  gPendingCR = FALSE;
  gFgColor = gColorLut[EFI_WHITE];
  gBgColor = gColorLut[EFI_BLACK];

  gGfxReady = TRUE;
  GfxClearScreen();
  return TRUE;
}

UINTN UiPrint(IN CONST CHAR16* Format, ...)
{
  VA_LIST marker;
  CHAR16 buffer[4096];

  VA_START(marker, Format);
  UnicodeVSPrint(buffer, sizeof(buffer), Format, marker);
  VA_END(marker);

  if (gGfxReady) {
    GfxWriteUnicode(buffer);
  } else {
    Print(L"%s", buffer);
  }

  return StrLen(buffer);
}

UINTN UiAsciiPrint(IN CONST CHAR8* Format, ...)
{
  VA_LIST marker;
  CHAR8 buffer[4096];

  VA_START(marker, Format);
  AsciiVSPrint(buffer, sizeof(buffer), Format, marker);
  VA_END(marker);

  if (gGfxReady) {
    GfxWriteAscii(buffer);
  } else {
    AsciiPrint("%a", buffer);
  }

  return AsciiStrLen(buffer);
}

// Set foreground and background colour from an EFI_TEXT_ATTRIBUTE value
// (bits 3:0 = foreground index, bits 6:4 = background index).
// Updates both the GOP colour globals and the UEFI ConOut attribute.
VOID UiSetAttribute(IN UINTN Attribute)
{
  UINTN fg = Attribute & 0x0F;
  UINTN bg = (Attribute >> 4) & 0x0F;

  if (gGfxReady) {
    if (fg < 16) {
      gFgColor = gColorLut[fg];
    }
    if (bg < 16) {
      gBgColor = gColorLut[bg];
    }
  }

  if (gST && gST->ConOut) {
    gST->ConOut->SetAttribute(gST->ConOut, Attribute);
  }
}

// ─── Public framebuffer drawing API ──────────────────────────────────────────

BOOLEAN UiGfxIsReady(VOID)
{
  return gGfxReady;
}

VOID UiGfxGetDimensions(OUT UINTN* Width, OUT UINTN* Height)
{
  if (Width)  *Width  = gFbWidth;
  if (Height) *Height = gFbHeight;
}

VOID UiGfxGetCellSize(OUT UINTN* CellW, OUT UINTN* CellH)
{
  if (CellW) *CellW = gCellW;
  if (CellH) *CellH = gCellH;
}

VOID UiGfxGetGlyphSize(OUT UINTN* GlyphW, OUT UINTN* GlyphH)
{
  if (GlyphW) *GlyphW = gGlyphW;
  if (GlyphH) *GlyphH = gGlyphH;
}

VOID UiGfxSetCursor(UINTN Col, UINTN Row)
{
  gCursorX = Col;
  gCursorY = Row;
}

VOID UiGfxFillRectRgb(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b)
{
  GfxFillRect(x, y, w, h, MakeColor(r, g, b));
}

VOID UiGfxSetPixel(UINTN x, UINTN y, UINT8 r, UINT8 g, UINT8 b)
{
  if (!gGfxReady || !gLfb || x >= gFbWidth || y >= gFbHeight) {
    return;
  }
  gLfb[y * gPitchPixels + x] = MakeColor(r, g, b);
}

// Scaled glyph renderer — same SSFN logic as GfxDrawGlyph but each source pixel
// becomes a (scale × scale) filled rectangle in the framebuffer.
static BOOLEAN GfxDrawGlyphScaled(UINT32 codepoint, UINTN x, UINTN y, UINT32 color, UINTN scale)
{
  return GfxDrawGlyphScaledXY(codepoint, x, y, color,
                              scale == 0 ? 1 : scale,
                              scale == 0 ? 1 : scale);
}

VOID UiGfxDrawGlyphScaled(UINT32 codepoint, UINTN x, UINTN y,
                           UINT8 r, UINT8 g, UINT8 b, UINTN scale)
{
  GfxDrawGlyphScaled(codepoint, x, y, MakeColor(r, g, b), scale == 0 ? 1 : scale);
}

VOID UiGfxGetCursor(OUT UINTN* Col, OUT UINTN* Row)
{
  if (Col) *Col = gCursorX;
  if (Row) *Row = gCursorY;
}

// Draw ASCII string at exact pixel coordinates without affecting the text cursor.
VOID UiGfxDrawAsciiAt(UINTN x, UINTN y, CONST CHAR8* str, UINT8 r, UINT8 g, UINT8 b)
{
  if (!str || !gGfxReady || !gLfb) {
    return;
  }
  UINT32 color = MakeColor(r, g, b);
  while (*str) {
    GfxDrawGlyphScaledXY((UINT32)(UINT8)*str, x, y, color,
                         gTextScaleX, gTextScaleY);
    x += gCellW;
    str++;
  }
}
