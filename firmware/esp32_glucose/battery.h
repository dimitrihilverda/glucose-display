// Battery telemetry for the JC3248W535C. The battery voltage is wired to
// GPIO 5 through a divider (measured * 1.72 = VBAT) -- community-verified
// ESPHome config for this exact board. The IP5306 on this board is the
// non-I2C variant, so the ADC is the only telemetry there is.
//
// Level comes from a LiPo discharge curve (linear interpolation between the
// same calibration points the ESPHome config uses); "charging" is inferred
// from the voltage sitting at charge level. No battery connected reads out
// of the plausible window and hides the whole thing.
#ifndef BATTERY_H
#define BATTERY_H

#define BAT_ADC_PIN 5
#define BAT_DIVIDER 1.72f
#define BAT_WINDOW  8          // rolling average over recent polls

static bool bat_present = false;
static int bat_pct = -1;
static float bat_volt = 0.0f;
static bool bat_charging = false, bat_full = false;

static float bat_win[BAT_WINDOW];
static int bat_win_n = 0, bat_win_i = 0;

// LiPo curve: voltage -> percent, straight from the community calibration.
static const float BAT_CURVE[][2] = {
  { 3.00f, 0.0f }, { 3.35f, 12.0f }, { 3.40f, 20.0f },
  { 3.60f, 60.0f }, { 4.00f, 90.0f }, { 4.10f, 100.0f },
};
#define BAT_POINTS 6

static float bat_curve_pct(float v) {
  if (v <= BAT_CURVE[0][0]) return 0.0f;
  if (v >= BAT_CURVE[BAT_POINTS - 1][0]) return 100.0f;
  for (int i = 1; i < BAT_POINTS; i++) {
    if (v <= BAT_CURVE[i][0]) {
      float x0 = BAT_CURVE[i - 1][0], y0 = BAT_CURVE[i - 1][1];
      float x1 = BAT_CURVE[i][0],     y1 = BAT_CURVE[i][1];
      return y0 + (v - x0) / (x1 - x0) * (y1 - y0);
    }
  }
  return 100.0f;
}

// One reading: 16 calibrated samples averaged, times the divider.
static float bat_read_volt() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
  return (mv / 16) / 1000.0f * BAT_DIVIDER;
}

static void battery_poll() {
  float v = bat_read_volt();
  bat_win[bat_win_i] = v;
  bat_win_i = (bat_win_i + 1) % BAT_WINDOW;
  if (bat_win_n < BAT_WINDOW) bat_win_n++;
  float sum = 0;
  for (int i = 0; i < bat_win_n; i++) sum += bat_win[i];
  bat_volt = sum / bat_win_n;

  // outside the plausible LiPo window: probably no battery attached
  bat_present = (bat_volt >= 2.8f && bat_volt <= 4.45f);
  if (!bat_present) { bat_pct = -1; return; }
  bat_pct = (int)(bat_curve_pct(bat_volt) + 0.5f);
  bat_full = bat_volt >= 4.15f && bat_pct >= 100;
  bat_charging = bat_volt >= 4.18f && !bat_full;
}

static void battery_begin() {
  analogReadResolution(12);
  battery_poll();
  Serial.printf("battery: adc gpio5 %.2fV -> %s\n", bat_volt,
                bat_present ? String(String(bat_pct) + "%").c_str() : "geen accu");
}

// Small battery glyph with fill level, a bolt while charging.
static void battery_draw(int x, int y, uint16_t fg, uint16_t warn) {
  if (!bat_present || bat_pct < 0) return;
  const int w = 30, h = 14;
  uint16_t col = (bat_pct <= 25 && !bat_charging && !bat_full) ? warn : fg;
  gfx->drawRoundRect(x, y, w, h, 3, col);
  gfx->fillRect(x + w, y + 4, 3, h - 8, col);          // nub
  int fill = (w - 6) * bat_pct / 100;
  if (fill > 0) gfx->fillRect(x + 3, y + 3, fill, h - 6, col);
  if (bat_charging) {                                   // little bolt
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y - 2, bx - 5, y + h / 2 + 1, bx + 1, y + h / 2 + 1, warn);
    gfx->fillTriangle(bx - 3, y + h + 2, bx + 5, y + h / 2 - 1, bx - 1, y + h / 2 - 1, warn);
  }
}

#endif
