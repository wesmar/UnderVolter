// ConsoleUi.c — Startup animation (two flashing lightning bolts + oscilloscope
//               trace + title color-wave) rendered directly to the GOP
//               framebuffer.  Also owns the countdown progress bar.
//               Falls back to an ASCII art banner when GOP is unavailable.
#include "ConsoleUi.h"
#include "UiConsole.h"
#include <Library/UefiBootServicesTableLib.h>

extern EFI_BOOT_SERVICES* gBS;

// ─── Sine LUT (64 entries, values 0-255, one full cycle) ─────────────────────
static const UINT8 kSin64[64] = {
  128, 140, 152, 165, 176, 187, 197, 206,
  214, 221, 227, 232, 236, 239, 241, 242,
  243, 242, 241, 239, 236, 232, 227, 221,
  214, 206, 197, 187, 176, 165, 152, 140,
  128, 115, 103,  90,  79,  68,  58,  49,
   41,  34,  28,  23,  19,  16,  14,  13,
   12,  13,  14,  16,  19,  23,  28,  34,
   41,  49,  58,  68,  79,  90, 103, 115,
};

static UINT8 SinU8(UINTN angle)
{
  return kSin64[angle & 63];
}

static UINT8 ClampU8(UINTN v)
{
  return (UINT8)(v > 255 ? 255 : v);
}

// ─── Lightning bolt shape ─────────────────────────────────────────────────────
// Grid: 16 wide x 28 tall (multiply by `scale` for real pixels).
// Wide 4-unit strokes, sharp diagonal upper prong, full crossbar, lower spike.
// RIGHT-FACING bolt: upper prong sweeps top-right to bottom-left,
// lower prong sweeps back to the far right.
typedef struct { UINT8 x, y, w, h; } BRECT;

static const BRECT kBolt[] = {
  // Upper shaft: solid, nearly vertical
  {  6,  0, 10,  3 },
  {  5,  3, 10,  3 },
  {  4,  6, 10,  3 },
  {  3,  9, 10,  4 },
  // The intense jagged crossbar (kink)
  {  2, 13, 14,  3 },
  // Lower spike: sharp and directed straight down
  {  7, 16,  8,  3 },
  {  6, 19,  6,  3 },
  {  5, 22,  4,  3 },
  {  4, 25,  2,  3 },
};
#define BOLT_SEGS   (sizeof(kBolt) / sizeof(kBolt[0]))
#define BOLT_GRID_W 16
#define BOLT_GRID_H 28

// ─── Draw functions: normal (right-facing) and mirrored (left-facing) ─────────

static VOID DrawBolt(UINTN bx, UINTN by, UINTN scale, UINT8 r, UINT8 g, UINT8 b)
{
  for (UINTN i = 0; i < BOLT_SEGS; i++) {
    UiGfxFillRectRgb(
      bx + (UINTN)kBolt[i].x * scale,
      by + (UINTN)kBolt[i].y * scale,
      (UINTN)kBolt[i].w * scale,
      (UINTN)kBolt[i].h * scale,
      r, g, b);
  }
}

// Mirrored: flip x so this bolt faces LEFT (for the left-side position).
static VOID DrawBoltMirror(UINTN bx, UINTN by, UINTN scale, UINT8 r, UINT8 g, UINT8 b)
{
  for (UINTN i = 0; i < BOLT_SEGS; i++) {
    UINTN mx = (UINTN)(BOLT_GRID_W - (UINTN)kBolt[i].x - (UINTN)kBolt[i].w);
    UiGfxFillRectRgb(
      bx + mx * scale,
      by + (UINTN)kBolt[i].y * scale,
      (UINTN)kBolt[i].w * scale,
      (UINTN)kBolt[i].h * scale,
      r, g, b);
  }
}

// Soft glow halo: 1-px border at ~20% brightness around each segment.
static VOID DrawBoltGlow(UINTN bx, UINTN by, UINTN scale, UINT8 r, UINT8 g, UINT8 b)
{
  UINT8 gr = r / 5;
  UINT8 gg = g / 5;
  UINT8 gb = b / 5;
  for (UINTN i = 0; i < BOLT_SEGS; i++) {
    UINTN sx = bx + (UINTN)kBolt[i].x * scale;
    UINTN sy = by + (UINTN)kBolt[i].y * scale;
    UINTN sw = (UINTN)kBolt[i].w * scale;
    UINTN sh = (UINTN)kBolt[i].h * scale;
    if (sx >= 2 && sy >= 2) {
      UiGfxFillRectRgb(sx - 1, sy - 1, sw + 2, 1,  gr, gg, gb);
      UiGfxFillRectRgb(sx - 1, sy + sh, sw + 2, 1, gr, gg, gb);
      UiGfxFillRectRgb(sx - 1, sy,      1,      sh, gr, gg, gb);
      UiGfxFillRectRgb(sx + sw, sy,     1,      sh, gr, gg, gb);
    }
  }
}

static VOID DrawBoltMirrorGlow(UINTN bx, UINTN by, UINTN scale, UINT8 r, UINT8 g, UINT8 b)
{
  UINT8 gr = r / 5;
  UINT8 gg = g / 5;
  UINT8 gb = b / 5;
  for (UINTN i = 0; i < BOLT_SEGS; i++) {
    UINTN mx = (UINTN)(BOLT_GRID_W - (UINTN)kBolt[i].x - (UINTN)kBolt[i].w);
    UINTN sx = bx + mx * scale;
    UINTN sy = by + (UINTN)kBolt[i].y * scale;
    UINTN sw = (UINTN)kBolt[i].w * scale;
    UINTN sh = (UINTN)kBolt[i].h * scale;
    if (sx >= 2 && sy >= 2) {
      UiGfxFillRectRgb(sx - 1, sy - 1, sw + 2, 1,  gr, gg, gb);
      UiGfxFillRectRgb(sx - 1, sy + sh, sw + 2, 1, gr, gg, gb);
      UiGfxFillRectRgb(sx - 1, sy,      1,      sh, gr, gg, gb);
      UiGfxFillRectRgb(sx + sw, sy,     1,      sh, gr, gg, gb);
    }
  }
}

// ─── Integer square root (Newton's method, no libc) ──────────────────────────
static UINTN ISqrt(UINTN n)
{
  if (n == 0) return 0;
  UINTN x = n, y = (n + 1) / 2;
  while (y < x) { x = y; y = (x + n / x) / 2; }
  return x;
}

// ─── Elliptic arc connecting the two bolts ───────────────────────────────────
// A single sine controls alternating arc visibility:
//   phase > 128  → only the top arc lights up (white-yellow)
//   phase < 128  → only the bottom arc lights up (gold-amber)
//   phase == 128 → both arcs dark (clean crossover with no overlap)
static VOID DrawEllipticArc(
    UINTN frame, UINTN x1, UINTN x2, UINTN midY,
    UINTN maxY, UINTN scale,
    UINT8 r, UINT8 g, UINT8 b)
{
  (VOID)r; (VOID)g; (VOID)b;

  if (x2 <= x1) return;

  UINTN cx = (x1 + x2) / 2;
  UINTN a  = (x2 - x1) / 2;
  if (a == 0) return;

  // Minor radius: keeps arc segments close to the midline
  UINTN bv = 10 * scale;
  if (bv < 8) bv = 8;

  // Single sine: phase > 128 → top arc, phase < 128 → bottom arc
  UINT8 phase = SinU8((frame * 7) & 63);

  UINT8 brightTop, brightBot;
  if (phase >= 128) {
    brightTop = (UINT8)((UINTN)(phase - 128) * 2);  // 0..230
    brightBot = 0;
  } else {
    brightTop = 0;
    brightBot = (UINT8)((UINTN)(128 - phase) * 2);  // 0..232
  }

  // Top arc: white-yellow; blue channel peaks to white at full brightness
  UINT8 rT = (UINT8)((UINTN)255 * brightTop / 255);
  UINT8 gT = (UINT8)((UINTN)220 * brightTop / 255);
  UINT8 bT = (UINT8)((UINTN)brightTop * brightTop / 255);
  UINT8 grT = rT / 7, ggT = gT / 7, gbT = bT / 7;

  // Bottom arc: gold-amber
  UINT8 rB  = (UINT8)((UINTN)200 * brightBot / 255);
  UINT8 gB  = (UINT8)((UINTN)100 * brightBot / 255);
  UINT8 grB = rB / 7, ggB = gB / 7;

  UINTN thick = (scale > 1) ? scale : 2;
  UINTN a2    = a * a;

  for (UINTN px = x1; px <= x2; px++) {
    INTN  dx  = (INTN)px - (INTN)cx;
    UINTN adx = (UINTN)(dx >= 0 ? dx : -dx);
    UINTN dx2 = adx * adx;
    if (dx2 > a2) continue;

    UINTN sinT = ISqrt((a2 - dx2) * (UINTN)1024 * (UINTN)1024 / a2);

    INTN topY = (INTN)midY - (INTN)(bv * sinT / 1024);
    INTN botY = (INTN)midY + (INTN)(bv * sinT / 1024);

    // Top arc — only rendered when brightTop > 0
    if (brightTop > 0 && topY >= 2 && (UINTN)topY + thick + 4 < maxY) {
      UiGfxFillRectRgb(px, (UINTN)topY - 2, 1, thick + 4, grT, ggT, gbT);
      UiGfxFillRectRgb(px, (UINTN)topY,     1, thick,     rT,  gT,  bT);
      if (brightTop > 180)
        UiGfxFillRectRgb(px, (UINTN)topY, 1, 1, 255, 255, 255);
    }
    // Bottom arc — only rendered when brightBot > 0
    if (brightBot > 0 && botY >= 2 && (UINTN)botY + thick + 4 < maxY) {
      UiGfxFillRectRgb(px, (UINTN)botY - 2, 1, thick + 4, grB, ggB, 0);
      UiGfxFillRectRgb(px, (UINTN)botY,     1, thick,     rB,  gB,  0);
      if (brightBot > 180)
        UiGfxFillRectRgb(px, (UINTN)botY, 1, 1, 255, 200, 0);
    }
  }
}

// ─── Two bolts + arc: the hero animation element ──────────────────────────────
// Left bolt (mirrored) at SW/4, right bolt at 3*SW/4, electric arc between.
static VOID DrawTwoBoltsWithArc(
    UINTN frame, UINTN SW, UINTN by, UINTN boltH,
    UINTN scale, UINT8 r, UINT8 g, UINT8 b)
{
  UINTN boltW = (UINTN)BOLT_GRID_W * scale;

  // Centre each bolt on its quarter-screen position
  UINTN leftCX  = SW / 4;
  UINTN rightCX = SW * 3 / 4;
  UINTN leftBoltX  = (leftCX  > boltW / 2) ? leftCX  - boltW / 2 : 0;
  UINTN rightBoltX = (rightCX > boltW / 2) ? rightCX - boltW / 2 : 0;

  // Bolts flash with a "power surge" logic: slow buildup, instant strike, then glow
  // Lowering the frequency (frame * 6) for fewer, but more meaningful strikes
  UINT8 phaseL = SinU8(frame * 6 + 0);
  UINT8 phaseR = SinU8(frame * 6 + 32);
  
  // High-contrast pulse for a "staccato" lightning feel
  UINTN pulseL = ((UINTN)phaseL * (UINTN)phaseL * (UINTN)phaseL) / (255 * 255);
  UINTN pulseR = ((UINTN)phaseR * (UINTN)phaseR * (UINTN)phaseR) / (255 * 255);

  // Core brightness (for white strike)
  UINT8 coreL = (UINT8)(pulseL * 255 / 255);
  UINT8 coreR = (UINT8)(pulseR * 255 / 255);

  // Afterglow (yellow-gold) component: lingers longer and stays bright
  UINT8 glowL = ClampU8(60 + (UINTN)phaseL * 195 / 255);
  UINT8 glowR = ClampU8(60 + (UINTN)phaseR * 195 / 255);

  // Arc pulses with chaotic energy
  UINT8 phaseA = SinU8(frame * 16 + 16);
  UINT8 arcCore = (UINT8)(((UINTN)phaseA * (UINTN)phaseA) / 255);

  // Arc endpoints: inner edges of each bolt at crossbar level (y=13 on grid)
  UINTN arcX1 = leftBoltX  + boltW + scale;
  UINTN arcX2 = (rightBoltX > scale) ? rightBoltX - scale : rightBoltX;
  UINTN arcY  = by + 13 * scale;           // crossbar level
  UINTN arcMaxY = by + boltH + 10;

  // Final colors: 
  // Strike peak: (255, 255, 255) - pure white core
  // Idle/Glow:   (255, 210, 0)   - warm golden yellow
  
  // Left Bolt Color
  UINT8 rL = 255;
  UINT8 gL = glowL; // Green follows the slower afterglow phase
  UINT8 bL = coreL; // Blue only at the peak of the strike for white core

  // Right Bolt Color
  UINT8 rR = 255;
  UINT8 gR = glowR;
  UINT8 bR = coreR;

  // Arc color
  UINT8 rA = 255;
  UINT8 gA = ClampU8(180 + (UINTN)arcCore * 75 / 255);
  UINT8 bA = arcCore;

  // Draw order: glow -> arc -> bolts
  DrawBoltMirrorGlow(leftBoltX,  by, scale, rL, gL, bL);
  DrawBoltGlow      (rightBoltX, by, scale, rR, gR, bR);
  DrawEllipticArc(frame, arcX1, arcX2, arcY, arcMaxY, scale, rA, gA, bA);
  DrawBoltMirror(leftBoltX,  by, scale, rL, gL, bL);
  DrawBolt      (rightBoltX, by, scale, rR, gR, bR);
}

// ─── Oscilloscope: 3 colour sine segments with gaps ──────────────────────────
// Screen divided into 3 zones (R, G, B).  Each zone: 2/3 sine + 1/3 flat gap.
// The flat gap is a dark horizontal line at cy.  Scroll speed: sw/64 px/frame.
static const UINT8 kOscR[3] = { 220,   0,  40 };
static const UINT8 kOscG[3] = {   0, 200,  40 };
static const UINT8 kOscB[3] = {  40,  80, 220 };

static VOID DrawOscilloscope(UINTN frame, UINTN cy, UINTN amp, UINTN sw, UINTN maxY)
{
  if (sw == 0) return;

  UINTN period = sw / 3;
  if (period == 0) return;
  UINTN segW = period * 2 / 3;
  if (segW == 0) segW = 1;

  // Scroll speed matches the original design: sw/64 px per frame
  UINTN scrollPx = (frame * (sw / 64 + 1)) % sw;

  for (UINTN px = 0; px < sw; px++) {
    UINTN pos    = (px + scrollPx) % sw;
    UINTN segIdx = (pos / period) % 3;   // % 3 chroni przy sw niebędącym wielokrotnością 3
    UINTN posInP = pos % period;

    if (posInP < segW) {
      // Coloured sine — 3 full cycles per segment
      UINTN angle = (posInP * 64 * 3 / segW) & 63;
      UINT8 sv    = SinU8(angle);
      INTN  dy    = ((INTN)sv - 128) * (INTN)amp / 128;
      INTN  pys   = (INTN)cy + dy;
      if (pys < 2 || pys + 4 >= (INTN)maxY) continue;
      UINTN py = (UINTN)pys;

      UINT8 r = kOscR[segIdx];
      UINT8 g = kOscG[segIdx];
      UINT8 b = kOscB[segIdx];

      if (py >= 2)
        UiGfxFillRectRgb(px, py - 1, 1, 2, r / 5, g / 5, b / 5);  // glow halo
      UiGfxFillRectRgb(px, py,       1, 2, r,      g,     b);       // core trace
    } else {
      // Gap — dark horizontal line at cy
      if (cy < maxY)
        UiGfxFillRectRgb(px, cy, 1, 1, 18, 18, 18);
    }
  }
}

// ─── Title "UNDERVOLTER" with per-character sine color wave ──────────────────
static const CHAR8 kTitle[] = "UNDERVOLTER";
#define TITLE_LEN 11

static UINTN GetTitleAdvance(UINTN scale)
{
  UINTN gw = 0, gh = 0;
  UiGfxGetGlyphSize(&gw, &gh);
  if (gw == 0) gw = 8;
  UINTN tracking = (scale >= 3) ? (scale / 2 + 1) : 1;
  return gw * scale + tracking;
}

static VOID DrawTitle(UINTN frame, UINTN titleX, UINTN titleY, UINTN scale)
{
  UINTN advance = GetTitleAdvance(scale);

  for (UINTN ci = 0; ci < TITLE_LEN; ci++) {
    UINTN angle = (frame * 3 + ci * 9) & 63;
    UINT8 sv    = SinU8(angle);

    UINT8 r = ClampU8((UINTN)sv / 2);
    UINT8 g = ClampU8(180 + (UINTN)sv * 55 / 255);
    UINT8 b = 255;

    UINTN cx = titleX + ci * advance;

    // Subtle depth glow: same glyph 1px up-left at ~15% brightness
    UiGfxDrawGlyphScaled((UINT32)kTitle[ci],
                          cx > 0 ? cx - 1 : cx,
                          titleY > 0 ? titleY - 1 : titleY,
                          r / 7, g / 7, b / 7,
                          scale);
    // Main glyph
    UiGfxDrawGlyphScaled((UINT32)kTitle[ci], cx, titleY, r, g, b, scale);
  }
}

// ─── Startup animation ────────────────────────────────────────────────────────
#define ANIM_FRAMES 60

// Render ANIM_FRAMES (60) frames at ~16 ms each to the GOP framebuffer, then
// leave a frozen final frame and print the version/author banner to the console.
// Falls back to a plain ASCII art header if no GOP framebuffer is available.
VOID RunStartupAnimation(VOID)
{
  if (!UiGfxIsReady()) {
    UiAsciiPrint(
      " _   _ _   _ ____  _____ ______     ______  _    _______ _____ ____  \n"
      "| | | | \\ | |  _ \\| ____|  _ \\ \\   / / __ \\| |  |__   __|  ___|  _ \\ \n"
      "| | | |  \\| | | | | |__ | |_) \\ \\ / / |  | | |     | |  | |_  | |_) |\n"
      "| | | | . ` | | | |  __||  _  / \\ V /| |  | | |     | |  |  _| |  __/ \n"
      "| |_| | |\\  | |__| | |___| | \\ \\  | | | |__| | |____ | |  | |___| |   \n"
      " \\___/|_| \\_|_____/|_____|_|  \\_\\ |_|  \\____/|______| |_|  |_____|_|   \n\n"
    );
    UiAsciiPrint(" v1.0.2  (C) 2026\n\n");
    return;
  }

  UINTN SW, SH;
  UiGfxGetDimensions(&SW, &SH);

  UINTN glyphH = 0;
  UiGfxGetGlyphSize(NULL, &glyphH);
  if (glyphH == 0) glyphH = 16;

  // ── Layout ────────────────────────────────────────────────────────────────

  // Top 46% of screen is the animation canvas
  UINTN animH = SH * 46 / 100;

  // Bolts: one at SW/4, one at 3*SW/4
  UINTN boltScale = (SH >= 900) ? 5 : (SH >= 700) ? 4 : 3;
  UINTN boltH     = (UINTN)BOLT_GRID_H * boltScale;
  UINTN boltY     = SH * 5 / 100;
  if (boltY < 10) boltY = 10;  // ensure glow halo never clips the top edge

  // Title: centred, below bolts — clamped to stay inside animH so that
  // UiClearAnimationArea() always erases the title before the voltage table.
  UINTN titleScale = (SH >= 900) ? 5 : (SH >= 700) ? 4 : 3;
  UINTN titleAdv   = GetTitleAdvance(titleScale);
  UINTN titleW     = TITLE_LEN * titleAdv;
  UINTN titleX     = (SW > titleW) ? (SW - titleW) / 2 : 0;
  UINTN titleH     = glyphH * titleScale + 4;   // +4: drop-shadow pixel
  // Position title in the upper third of the gap below the bolts.
  UINTN spaceBelowBolts = animH - (boltY + boltH);
  UINTN titleY = boltY + boltH + (spaceBelowBolts - titleH) / 3;
  if (titleY + titleH > animH) {
    titleY = (animH > titleH + 4) ? animH - titleH - 4 : 0;
  }

  // Oscilloscope: below title, inside animH
  UINTN waveY   = titleY + glyphH * titleScale + SH * 2 / 100;
  UINTN waveAmp = SH * 3 / 100;
  if (waveAmp < 12) waveAmp = 12;
  if (waveY + waveAmp + 10 >= animH) {
    waveY = animH - waveAmp - 10;
  }

  // ── Animation loop ────────────────────────────────────────────────────────

  for (UINTN frame = 0; frame < ANIM_FRAMES; frame++) {

    // Background: deep navy
    UiGfxFillRectRgb(0, 0, SW, animH, 0, 0, 10);

    // Oscilloscope trace (drawn first -- stays behind bolts)
    DrawOscilloscope(frame, waveY, waveAmp, SW, animH);

    // Two bolts + electric arc: golden-yellow
    DrawTwoBoltsWithArc(frame, SW, boltY, boltH, boltScale, 255, 200, 0);

    // Title with electric-blue color wave
    DrawTitle(frame, titleX, titleY, titleScale);

    // ~16 ms per frame
    gBS->Stall(16000);
  }

  // ── Final frozen frame ────────────────────────────────────────────────────
  UiGfxFillRectRgb(0, 0, SW, animH, 0, 0, 10);
  DrawOscilloscope(ANIM_FRAMES, waveY, waveAmp, SW, animH);
  DrawTwoBoltsWithArc(16, SW, boltY, boltH, boltScale, 255, 230, 0);
  DrawTitle(16, titleX, titleY, titleScale);

  // Clear the animation zone and reposition the text cursor for normal output
  UiClearAnimationArea();
  UiGfxSetCursor(0, 0);

  UiSetAttribute(EFI_LIGHTCYAN);
  UiPrint(L" v1.0.2");
  UiSetAttribute(EFI_LIGHTGREEN);
  UiPrint(
    L"  |  Marek Weso\x0142owski (wesmar)"
    L"  |  tel. +48 607-440-283"
    L"  |  marek@wesolowski.eu.org\n"
  );
  UiSetAttribute(EFI_YELLOW);
  UiPrint(
    L" IMPORTANT: UnderVolter is about to program custom CPU voltage/frequency settings.\n"
  );
  UiSetAttribute(EFI_WHITE);
  UiPrint(
    L" Programming starts shortly. Press ESC during the countdown below to abort.\n\n"
  );
  UiSetAttribute(EFI_WHITE);
}

// ─── GOP gradient countdown bar ───────────────────────────────────────────────
#define BAR_W 600
#define BAR_H  22

// Clear the upper half of the screen where the animation was drawn
VOID UiClearAnimationArea(VOID)
{
  if (!UiGfxIsReady()) {
    return;
  }
  
  UINTN SW, SH;
  UiGfxGetDimensions(&SW, &SH);
  UINTN animH = SH * 46 / 100;
  
  // Paint over the entire animation zone with black
  UiGfxFillRectRgb(0, 0, SW, animH, 0, 0, 0);
}

// Draw a countdown progress bar centred near the bottom of the screen.
// ProgressPercentage 0..100; bar colour transitions green→yellow→red.
// Falls back to an ASCII |===   | text bar when GOP is unavailable.
VOID DrawProgressBar(IN UINTN ProgressPercentage)
{
  if (!UiGfxIsReady()) {
    UINTN fill = PROGRESS_BAR_WIDTH * ProgressPercentage / 100;
    UiPrint(L"|");
    for (UINTN i = 0; i < PROGRESS_BAR_WIDTH; i++) {
      UiPrint(i < fill ? L"=" : L" ");
    }
    UiPrint(L"| %d%%\r", ProgressPercentage);
    return;
  }

  UINTN SW, SH;
  UiGfxGetDimensions(&SW, &SH);

  UINTN barX = (SW > BAR_W) ? (SW - BAR_W) / 2 : 0;
  UINTN barY = (SH > 100) ? SH - 80 : SH / 2;

  UINTN filled = (UINTN)BAR_W * ProgressPercentage / 100;

  // Dark background track
  UiGfxFillRectRgb(barX, barY, BAR_W, BAR_H, 22, 22, 32);

  // Filled portion: green (0%) -> yellow (50%) -> red (100%)
  if (filled > 0) {
    UINT8 r = ClampU8(ProgressPercentage * 2 + 20);
    UINT8 g = ClampU8(230 - ProgressPercentage * 2);
    UINT8 b = 0;
    UiGfxFillRectRgb(barX, barY, filled, BAR_H, r, g, b);
  }

  // Thin border
  UiGfxFillRectRgb(barX,              barY,             BAR_W, 1, 60, 60, 90);
  UiGfxFillRectRgb(barX,              barY + BAR_H - 1, BAR_W, 1, 60, 60, 90);
  UiGfxFillRectRgb(barX,              barY,             1, BAR_H, 60, 60, 90);
  UiGfxFillRectRgb(barX + BAR_W - 1, barY,             1, BAR_H, 60, 60, 90);
}
