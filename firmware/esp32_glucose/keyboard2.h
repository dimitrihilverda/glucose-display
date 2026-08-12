// Modal on-screen keyboard with three layers (abc / ABC / 123+symbols), for
// WiFi passwords and Dexcom credentials. Unlike the barista keyboard this one
// is blocking: kb_input() runs its own touch loop and returns when the user
// hits OK (true) or annuleer (false). Nothing else needs the CPU meanwhile.
//
// Layout, 480x320: title + typed value on top, 4x48px key rows at the bottom.
//   row0  10 keys
//   row1   9 keys, half-key indent
//   row2  [shift] + 7 keys + [backspace]
//   row3  [layer] [space] [annuleer] [OK]
#ifndef KEYBOARD2_H
#define KEYBOARD2_H

// Portrait metrics: 10 columns of 32px across the 320px width, four 52px
// rows anchored to the bottom of the 480px screen.
#define KB_ROWS_Y0 (SCR_H - 4 * KB_KEY_H)
#define KB_KEY_H   52
#define KB_KEY_W   32

static const char *KB_R0[3] = {"qwertyuiop", "QWERTYUIOP", "1234567890"};
static const char *KB_R1[3] = {"asdfghjkl",  "ASDFGHJKL",  "@#&*-+=_'"};
static const char *KB_R2[3] = {"zxcvbnm",    "ZXCVBNM",    ".,!?/()"};

static int kb_layer = 0;

static void kb2_key(int x, int y, int w, const char *label, bool accent, bool dim) {
  uint16_t bg = accent ? TH_ACCENT : TH_PANEL;
  gfx->fillRoundRect(x + 2, y + 2, w - 4, KB_KEY_H - 4, 6, bg);
  font_med();
  gfx->setTextColor(accent ? TH_BG : (dim ? TH_DIM : TH_TEXT));
  gfx->setCursor(x + (w - text_w(label)) / 2, y + KB_KEY_H / 2 + 9);
  gfx->print(label);
}

static void kb2_draw(const char *title, const char *val, bool mask) {
  gfx->fillScreen(TH_BG);
  font_small();
  gfx->setTextColor(TH_DIM);
  gfx->setCursor(MARGIN, 22);
  gfx->print(title);
  // typed value: show the tail that fits one line, masked except the
  // trailing character so you can see what you just hit
  char shown[28];
  int n = (int)strlen(val);
  int m = min(n, 24);
  for (int i = 0; i < m; i++)
    shown[i] = (mask && i < m - 1) ? '*' : val[n - m + i];
  shown[m] = '\0';
  font_med();
  gfx->setTextColor(TH_TEXT);
  gfx->setCursor(MARGIN, 70);
  gfx->print(shown);
  gfx->setTextColor(TH_ACCENT);
  gfx->print("_");

  const int y0 = KB_ROWS_Y0;
  char one[2] = {0, 0};
  for (int i = 0; KB_R0[kb_layer][i]; i++) {
    one[0] = KB_R0[kb_layer][i];
    kb2_key(i * KB_KEY_W, y0, KB_KEY_W, one, false, false);
  }
  for (int i = 0; KB_R1[kb_layer][i]; i++) {
    one[0] = KB_R1[kb_layer][i];
    kb2_key(16 + i * KB_KEY_W, y0 + KB_KEY_H, KB_KEY_W, one, false, false);
  }
  kb2_key(0, y0 + 2 * KB_KEY_H, 48, kb_layer == 1 ? "ab" : "AB", kb_layer == 1, kb_layer == 2);
  for (int i = 0; KB_R2[kb_layer][i]; i++) {
    one[0] = KB_R2[kb_layer][i];
    kb2_key(48 + i * KB_KEY_W, y0 + 2 * KB_KEY_H, KB_KEY_W, one, false, false);
  }
  kb2_key(SCR_W - 48, y0 + 2 * KB_KEY_H, 48, "<", false, false);
  kb2_key(0, y0 + 3 * KB_KEY_H, 64, kb_layer == 2 ? "abc" : "123", false, false);
  kb2_key(64, y0 + 3 * KB_KEY_H, 128, "spatie", false, false);
  kb2_key(192, y0 + 3 * KB_KEY_H, 64, "stop", false, true);
  kb2_key(256, y0 + 3 * KB_KEY_H, 64, "OK", true, false);
  gfx->flush();
}

// Hit-test: printable char, or '\b' bksp, '\x01' shift, '\x02' layer,
// '\n' OK, '\x1b' cancel, 0 dead space.
static char kb2_hit(int sx, int sy) {
  if (sy < KB_ROWS_Y0) return 0;
  int row = (sy - KB_ROWS_Y0) / KB_KEY_H;
  switch (row) {
    case 0: {
      int c = sx / KB_KEY_W;
      if (c >= 0 && c < (int)strlen(KB_R0[kb_layer])) return KB_R0[kb_layer][c];
      return 0;
    }
    case 1: {
      int c = (sx - 16) / KB_KEY_W;
      if (sx >= 16 && c < (int)strlen(KB_R1[kb_layer])) return KB_R1[kb_layer][c];
      return 0;
    }
    case 2: {
      if (sx < 48) return '\x01';
      if (sx >= SCR_W - 48) return '\b';
      int c = (sx - 48) / KB_KEY_W;
      if (c < (int)strlen(KB_R2[kb_layer])) return KB_R2[kb_layer][c];
      return 0;
    }
    case 3:
      if (sx < 64) return '\x02';
      if (sx < 192) return ' ';
      if (sx < 256) return '\x1b';
      return '\n';
  }
  return 0;
}

// Modal entry. `out` holds the initial value and receives the result.
static bool kb_input(const char *title, char *out, int outlen, bool mask) {
  int len = (int)strlen(out);
  kb_layer = 0;
  touch_irq = false;
  uint32_t arm = millis() + 400;         // the tap that opened us is not a key
  kb2_draw(title, out, mask);
  for (;;) {
    delay(20);
    if (!touch_tapped() || millis() < arm) continue;
    char c = kb2_hit(touch_sx, touch_sy);
    if (!c) continue;
    beep_click(cfg.volume);
    if (c == '\n') { touch_irq = false; return true; }
    if (c == '\x1b') { touch_irq = false; return false; }
    if (c == '\x01') { kb_layer = (kb_layer == 1) ? 0 : 1; }
    else if (c == '\x02') { kb_layer = (kb_layer == 2) ? 0 : 2; }
    else if (c == '\b') { if (len > 0) out[--len] = '\0'; }
    else if (len < outlen - 1) { out[len++] = c; out[len] = '\0'; }
    kb2_draw(title, out, mask);
  }
}

#endif
