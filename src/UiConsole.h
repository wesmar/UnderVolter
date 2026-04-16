// UiConsole.h — GOP framebuffer text/graphics console: SSFN glyph rendering,
//               ANSI-style color attributes, cursor management, and pixel drawing API.
#pragma once

#include <Uefi.h>

BOOLEAN UiConsoleInit(IN EFI_SYSTEM_TABLE* SystemTable);
UINTN   UiPrint(IN CONST CHAR16* Format, ...);
UINTN   UiAsciiPrint(IN CONST CHAR8* Format, ...);
VOID    UiSetAttribute(IN UINTN Attribute);

// Direct framebuffer drawing API
BOOLEAN UiGfxIsReady(VOID);
VOID    UiGfxGetDimensions(OUT UINTN* Width, OUT UINTN* Height);
VOID    UiGfxGetCellSize(OUT UINTN* CellW, OUT UINTN* CellH);
VOID    UiGfxGetGlyphSize(OUT UINTN* GlyphW, OUT UINTN* GlyphH);
VOID    UiGfxSetCursor(UINTN Col, UINTN Row);
VOID    UiGfxFillRectRgb(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b);
VOID    UiGfxSetPixel(UINTN x, UINTN y, UINT8 r, UINT8 g, UINT8 b);
VOID    UiGfxDrawGlyphScaled(UINT32 codepoint, UINTN x, UINTN y, UINT8 r, UINT8 g, UINT8 b, UINTN scale);
VOID    UiGfxGetCursor(OUT UINTN* Col, OUT UINTN* Row);
VOID    UiGfxDrawAsciiAt(UINTN x, UINTN y, CONST CHAR8* str, UINT8 r, UINT8 g, UINT8 b);
