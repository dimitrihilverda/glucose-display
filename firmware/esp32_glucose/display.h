// Display core for the glucose kiosk: the JC3248W535C QSPI panel through a
// full-frame canvas, same proven stack as the demo firmwares (AXS15231B has
// no hardware rotation and unreliable partial writes; the canvas plus a full
// flush sidesteps both). 40MHz QSPI: faster shimmers under load.
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino_GFX_Library.h>

// Dark theme tokens for the setup screens (RGB565).
#define TH_BG      0x10C4   // #101820 near-black blue
#define TH_PANEL   0x322B   // #33475B slate
#define TH_TEXT    0xFFFF
#define TH_ACCENT  0xE2B0   // #E75480 pink -- the house colour now
#define TH_DIM     0x9515   // #90A0AC secondary text

#define QSPI_BL   1
#define QSPI_CS   45
#define QSPI_SCK  47
#define QSPI_D0   21
#define QSPI_D1   48
#define QSPI_D2   40
#define QSPI_D3   39

#define PANEL_W 320
#define PANEL_H 480
#define TFT_ROTATION 0        // portrait: a desk display stands upright
#define SCR_W 320
#define SCR_H 480
#define MARGIN 4
#define TFT_TEXTSIZE 2
#define CW 12
#define CH 16
#define LINE_H 18
#define COLS ((SCR_W - 2 * MARGIN) / CW)

// Glucose status colors, next to the AiMELO theme tokens.
#define GL_OK    TH_ACCENT      // in range: lime
#define GL_HIGH  0xFD20         // orange
#define GL_LOW   0xF986         // red/pink, urgent

// Light Dexcom-app-style palette for the main screen (setup screens stay dark).
#define DX_BG    0xF79E         // page gray
#define DX_CARD  0xFFFF         // white card / circle
#define DX_TEXT  0x0000
#define DX_GRAY  0x8410
#define DX_BAND  0xE73D         // target-range band
#define DX_YEL   0xFE60         // high line / high circle
#define DX_RED   0xF9C6         // low line / low circle
#define DX_PINK  0xE2B0         // the owner's name
#define DX_PINKD 0xC18C         // heart
#define DX_GREEN 0x362B         // in-range share of the TIR bar
#define NT_AMBER 0xFD80         // night clock digits on black

static Arduino_DataBus *qspi_bus =
    new Arduino_ESP32QSPI(QSPI_CS, QSPI_SCK, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
static Arduino_GFX *panel =
    new Arduino_AXS15231B(qspi_bus, GFX_NOT_DEFINED, 0, false, PANEL_W, PANEL_H);
static Arduino_Canvas *gfx =
    new Arduino_Canvas(PANEL_W, PANEL_H, panel, 0, 0, TFT_ROTATION);

// PWM backlight, 0-100%. Used at 100 by day and a low glow at night; alarms
// and taps bring it back to full.
static void display_backlight(uint8_t pct) {
  static uint8_t last = 255;
  if (pct == last) return;
  static bool init = false;
  if (!init) { ledcAttach(QSPI_BL, 5000, 8); init = true; }
  ledcWrite(QSPI_BL, (uint32_t)255 * pct / 100);
  last = pct;
}

static bool display_begin() {
  display_backlight(100);
  if (!gfx->begin(40 * 1000 * 1000)) return false;
  gfx->fillScreen(TH_BG);
  gfx->setTextWrap(false);
  gfx->flush();
  return true;
}

// Modern proportional fonts (fonts.h, generated from Segoe UI). GFX fonts
// draw from the BASELINE and ignore the background colour, so callers paint
// their own background and pass a baseline y.
#include "fonts.h"
static void font_big()    { gfx->setFont(&GlucoseBig); gfx->setTextSize(1); }
static void font_med()    { gfx->setFont(&UiMed);      gfx->setTextSize(1); }
static void font_small()  { gfx->setFont(&UiSmall);    gfx->setTextSize(1); }
static void font_px(int s){ gfx->setFont((const GFXfont *)nullptr); gfx->setTextSize(s); }
static void font_script(){ gfx->setFont(&NameScript);  gfx->setTextSize(1); }
static int text_w(const char *s) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds((char *)s, 0, 100, &x1, &y1, &w, &h);
  return (int)w;
}

// A little heart, drawn from primitives so it works at any size and colour.
static void draw_heart(int cx, int cy, int s, uint16_t color) {
  gfx->fillTriangle(cx - s, cy, cx + s, cy, cx, cy + (s * 13) / 10, color);
  gfx->fillCircle(cx - s / 2, cy - s / 3, (s + 1) / 2 + 1, color);
  gfx->fillCircle(cx + s / 2, cy - s / 3, (s + 1) / 2 + 1, color);
}

// The owner's name in script with a heart; returns nothing, draws at a
// baseline. Works on both the dark and the light theme.
static void draw_name(int x, int baseline, uint16_t color, uint16_t heart) {
  font_script();
  gfx->setTextColor(color);
  gfx->setCursor(x, baseline);
  gfx->print(cfg.display_name);
  draw_heart(x + text_w(cfg.display_name) + 16, baseline - 12, 8, heart);
}

// Header for the setup screens: the owner's name in script, pink.
static void display_header(const char *label) {
  draw_name(MARGIN + 6, 40, TH_ACCENT, TH_ACCENT);
  font_small();
  gfx->setTextColor(TH_DIM);
  gfx->setCursor(SCR_W - MARGIN - text_w(label), 30);
  gfx->print(label);
  gfx->drawFastHLine(0, 52, SCR_W, TH_ACCENT);
}

// Legacy-shaped helper: y is still the old top-ish coordinate; size 1 maps to
// the small font, anything larger to the medium one.
static void display_centred(const char *s, int y, int size, uint16_t color) {
  if (size <= 1) font_small(); else font_med();
  gfx->setTextColor(color);
  gfx->setCursor((SCR_W - text_w(s)) / 2, y + (size <= 1 ? 13 : 20));
  gfx->print(s);
}

#endif
