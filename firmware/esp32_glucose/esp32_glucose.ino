// Glucose kiosk for the Guition JC3248W535C: a native Dexcom Share client.
// Big current value with trend arrow, six-hour sparkline, speaker alarms on
// lows/highs, on-screen WiFi setup and Dexcom login, settings in NVS.
//
// NOT A MEDICAL DEVICE. Readings arrive via Dexcom Share with the usual
// 5-minute cadence; treatment decisions belong on the official Dexcom app.

#include <WiFi.h>
#include <time.h>
#include "config.h"
#include "display.h"
#include "touch.h"
#include "beeps.h"
#include "keyboard2.h"
#include "dexcom.h"

// ---- state ------------------------------------------------------------------
#define HIST_MAX 72                    // 6 hours of 5-minute readings
static Egv hist[HIST_MAX];
static int hist_n = 0;
static uint32_t last_fetch_ms = 0;
static uint32_t last_draw_ms = 0;
static time_t last_alarm_t = 0;        // reading timestamp we last alarmed on
static uint32_t snooze_until_ms = 0;
static bool net_ok = false, dex_ok = false;

static float to_mmol(int mgdl) { return mgdl / 18.016f; }
static float shown_value(int mgdl) { return cfg.use_mmol ? to_mmol(mgdl) : (float)mgdl; }

// ---- small ui helpers ---------------------------------------------------------
static void btn(int x, int y, int w, int h, const char *label, bool accent) {
  gfx->fillRoundRect(x, y, w, h, 8, accent ? TH_ACCENT : TH_PANEL);
  font_med();
  gfx->setTextColor(accent ? TH_BG : TH_TEXT);
  gfx->setCursor(x + (w - text_w(label)) / 2, y + h / 2 + 9);
  gfx->print(label);
}
static bool in(int x, int y, int w, int h) {
  return touch_sx >= x && touch_sx < x + w && touch_sy >= y && touch_sy < y + h;
}
// Wait (blocking) for a debounced tap; returns when one landed.
static void wait_tap() {
  touch_irq = false;
  for (;;) { delay(20); if (touch_tapped()) return; }
}

// ---- disclaimer ---------------------------------------------------------------
static void screen_disclaimer() {
  while (!cfg.accepted) {
    gfx->fillScreen(TH_BG);
    display_header("glucose");
    display_centred("Dit is GEEN medisch", 110, 2, TH_TEXT);
    display_centred("hulpmiddel.", 136, 2, TH_TEXT);
    font_med();
    gfx->setTextColor(TH_DIM);
    gfx->setCursor(10, 208); gfx->print("Waarden lopen via Dexcom");
    gfx->setCursor(10, 244); gfx->print("Share en kunnen achter-");
    gfx->setCursor(10, 280); gfx->print("lopen. Beslissingen neem");
    gfx->setCursor(10, 316); gfx->print("je op je officiele app.");
    btn(60, 360, 200, 60, "begrepen", true);
    gfx->flush();
    wait_tap();
    if (in(60, 360, 200, 60)) {
      beep_click(cfg.volume);
      cfg.accepted = true;
      config_save();
    }
  }
}

// ---- wifi ---------------------------------------------------------------------
static bool wifi_connect_ui(const char *ssid, const char *pass) {
  gfx->fillScreen(TH_BG);
  display_header("wifi");
  display_centred("verbinden met", 200, 2, TH_DIM);
  char nm[25];
  strncpy(nm, ssid, 24); nm[24] = '\0';
  display_centred(nm, 230, 2, TH_TEXT);
  gfx->flush();
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, pass);
  for (int i = 0; i < 60; i++) {           // 15 s
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(250);
  }
  return false;
}

static void screen_wifi_setup() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("wifi");
    display_centred("netwerken zoeken...", 150, 2, TH_DIM);
    gfx->flush();
    int n = WiFi.scanNetworks();
    gfx->fillScreen(TH_BG);
    display_header("wifi");
    font_med();
    gfx->setTextColor(TH_DIM);
    gfx->setCursor(MARGIN, 78); gfx->print("kies je netwerk:");
    int shown = min(n, 8);
    for (int i = 0; i < shown; i++) {
      int y = 90 + i * 42;
      gfx->fillRoundRect(4, y, 312, 36, 6, TH_PANEL);
      font_med();
      gfx->setTextColor(TH_TEXT);
      gfx->setCursor(14, y + 27);
      char nm[19];
      strncpy(nm, WiFi.SSID(i).c_str(), 18); nm[18] = '\0';
      gfx->print(nm);
      // signal bars
      int bars = map(constrain(WiFi.RSSI(i), -90, -40), -90, -40, 1, 4);
      for (int b = 0; b < 4; b++)
        gfx->fillRect(272 + b * 9, y + 26 - b * 5, 6, 5 + b * 5,
                      b < bars ? TH_ACCENT : TH_BG);
    }
    btn(4, 430, 312, 44, "opnieuw zoeken", false);
    gfx->flush();
    wait_tap();
    if (in(4, 430, 312, 44)) { beep_click(cfg.volume); continue; }
    for (int i = 0; i < shown; i++) {
      int y = 90 + i * 42;
      if (!in(4, y, 312, 36)) continue;
      beep_click(cfg.volume);
      char ssid[33], pass[65] = "";
      strncpy(ssid, WiFi.SSID(i).c_str(), 32); ssid[32] = '\0';
      bool open_net = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      if (!open_net && !kb_input("wifi-wachtwoord:", pass, sizeof(pass), true))
        break;
      if (wifi_connect_ui(ssid, pass)) {
        strcpy(cfg.wifi_ssid, ssid);
        strcpy(cfg.wifi_pass, pass);
        config_save();
        beep_ok(cfg.volume);
        return;
      }
      gfx->fillScreen(TH_BG);
      display_header("wifi");
      display_centred("verbinden mislukt", 210, 2, GL_LOW);
      display_centred("(tik voor opnieuw)", 250, 2, TH_DIM);
      gfx->flush();
      wait_tap();
      break;
    }
  }
}

// ---- dexcom login ---------------------------------------------------------------
static void screen_dex_login() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("dexcom");
    font_small();
    gfx->setTextColor(TH_DIM);
    gfx->setCursor(MARGIN, 76); gfx->print("log in met je Dexcom-account");
    gfx->setCursor(MARGIN, 100); gfx->print("(Share aan + minstens 1 volger)");

    char ubtn[26];
    snprintf(ubtn, sizeof(ubtn), "%s", cfg.dex_user[0] ? cfg.dex_user : "gebruikersnaam...");
    if (strlen(ubtn) > 22) strcpy(ubtn + 19, "...");
    btn(4, 150, 312, 48, ubtn, false);
    btn(4, 206, 312, 48, cfg.dex_pass[0] ? "wachtwoord: ******" : "wachtwoord...", false);
    btn(4, 262, 150, 48, cfg.region_ous ? "server: EU" : "server: VS", false);
    btn(4, 340, 312, 56, "inloggen", true);
    if (dex_last_error[0]) {
      font_px(1);
      gfx->setTextColor(GL_LOW, TH_BG);
      gfx->setCursor(MARGIN, 430);
      gfx->print(dex_last_error);
    }
    gfx->flush();
    wait_tap();
    if (in(4, 150, 312, 48)) {
      beep_click(cfg.volume);
      kb_input("dexcom gebruikersnaam:", cfg.dex_user, sizeof(cfg.dex_user), false);
    } else if (in(4, 206, 312, 48)) {
      beep_click(cfg.volume);
      kb_input("dexcom wachtwoord:", cfg.dex_pass, sizeof(cfg.dex_pass), true);
    } else if (in(4, 262, 150, 48)) {
      beep_click(cfg.volume);
      cfg.region_ous = !cfg.region_ous;
    } else if (in(4, 340, 312, 56) && cfg.dex_user[0] && cfg.dex_pass[0]) {
      beep_click(cfg.volume);
      gfx->fillScreen(TH_BG);
      display_header("dexcom");
      display_centred("inloggen...", 230, 2, TH_DIM);
      gfx->flush();
      if (dex_login()) {
        config_save();
        beep_ok(cfg.volume);
        return;
      }
    }
  }
}

// ---- main screen -----------------------------------------------------------------
static void draw_trend_arrow(int cx, int cy, int8_t trend, uint16_t color) {
  // trend: 1 up-up, 2 up, 3 45up, 4 flat, 5 45down, 6 down, 7 down-down
  float ang;
  switch (trend) {
    case 1: case 2: ang = -90; break;
    case 3: ang = -45; break;
    case 4: ang = 0; break;
    case 5: ang = 45; break;
    case 6: case 7: ang = 90; break;
    default:
      font_med();
      gfx->setTextColor(DX_GRAY);
      gfx->setCursor(cx - 7, cy + 9);
      gfx->print("?");
      return;
  }
  float r = ang * 3.14159f / 180.0f, dx = cosf(r), dy = sinf(r);
  int n = (trend == 1 || trend == 7) ? 2 : 1;   // double arrows for the extremes
  for (int k = 0; k < n; k++) {
    int off = (n == 2) ? (k == 0 ? -10 : 10) : 0;
    int ox = cx - (int)(dy * off), oy = cy + (int)(dx * off);
    int x0 = ox - (int)(dx * 22), y0 = oy - (int)(dy * 22);
    int x1 = ox + (int)(dx * 12), y1 = oy + (int)(dy * 12);
    for (int t = -2; t <= 2; t++)
      gfx->drawLine(x0 - (int)(dy * t), y0 + (int)(dx * t),
                    x1 - (int)(dy * t), y1 + (int)(dx * t), color);
    gfx->fillTriangle(ox + (int)(dx * 26), oy + (int)(dy * 26),
                      x1 - (int)(dy * 9), y1 + (int)(dx * 9),
                      x1 + (int)(dy * 9), y1 - (int)(dx * 9), color);
  }
}

static uint16_t value_color(int mgdl) {
  float v = to_mmol(mgdl);
  if (v < cfg.low_mmol) return GL_LOW;
  if (v > cfg.high_mmol) return GL_HIGH;
  return GL_OK;
}

static void draw_main() {
  // Dexcom-app look: light page, white value circle with the trend arrow at
  // its side, then a white graph card with the target band and hour axis.
  gfx->fillScreen(DX_BG);
  time_t now = time(nullptr);

  // "Chantie" where the app shows its wordmark
  font_med();
  gfx->setTextColor(DX_GRAY);
  gfx->setCursor((SCR_W - text_w("Chantie")) / 2, 36);
  gfx->print("Chantie");
  if (!net_ok) {
    font_small();
    gfx->setTextColor(DX_RED);
    gfx->setCursor(MARGIN, 18);
    gfx->print("geen wifi");
  }

  if (hist_n == 0) {
    font_med();
    gfx->setTextColor(DX_GRAY);
    const char *m = dex_ok ? "wachten op data..." : "geen Dexcom-verbinding";
    gfx->setCursor((SCR_W - text_w(m)) / 2, 170);
    gfx->print(m);
    if (dex_last_error[0]) {
      font_px(1);
      gfx->setTextColor(DX_GRAY, DX_BG);
      gfx->setCursor(MARGIN, 200);
      gfx->print(dex_last_error);
    }
    gfx->flush();
    return;
  }

  const Egv &cur = hist[0];
  int age_min = (int)((now - cur.t) / 60);
  bool stale = age_min > 12;
  float vm = to_mmol(cur.mgdl);
  bool is_low = vm < cfg.low_mmol, is_high = vm > cfg.high_mmol;

  // the value circle: white in range, yellow high, red low (app idiom)
  const int ccx = 140, ccy = 160, R = 88;
  uint16_t cfill = stale ? DX_BG : (is_low ? DX_RED : (is_high ? DX_YEL : DX_CARD));
  uint16_t ctext = is_low && !stale ? DX_CARD : DX_TEXT;
  gfx->fillCircle(ccx + 2, ccy + 4, R, DX_BAND);       // soft shadow
  gfx->fillCircle(ccx, ccy, R, cfill);
  if (stale || cfill == DX_CARD || cfill == DX_BG)
    gfx->drawCircle(ccx, ccy, R, DX_BAND);

  char val[12];
  if (cfg.use_mmol) snprintf(val, sizeof(val), "%.1f", vm);
  else snprintf(val, sizeof(val), "%d", cur.mgdl);
  font_big();
  gfx->setTextColor(stale ? DX_GRAY : ctext);
  gfx->setCursor(ccx - text_w(val) / 2, ccy + 22);
  gfx->print(val);
  font_med();
  gfx->setTextColor(stale ? DX_GRAY : ctext);
  const char *unit = cfg.use_mmol ? "mmol/L" : "mg/dL";
  gfx->setCursor(ccx - text_w(unit) / 2, ccy + 62);
  gfx->print(unit);

  // trend arrow at the circle's right, app-style
  draw_trend_arrow(ccx + R + 42, ccy, stale ? -1 : cur.trend, DX_TEXT);

  // age, small and quiet; loud only when the data is old
  char sub[32];
  snprintf(sub, sizeof(sub), "%d min geleden", age_min);
  if (stale) {
    font_med();
    gfx->setTextColor(DX_RED);
  } else {
    font_small();
    gfx->setTextColor(DX_GRAY);
  }
  gfx->setCursor((SCR_W - text_w(sub)) / 2, 278);
  gfx->print(sub);

  // graph card, three-hour window like the app's default
  const int cx0 = 8, cy0 = 288, cx1 = SCR_W - 8, cy1 = 472;
  gfx->fillRoundRect(cx0, cy0, cx1 - cx0, cy1 - cy0, 12, DX_CARD);
  // "..." = settings, top-right of the card like the app
  font_med();
  gfx->setTextColor(DX_GRAY);
  gfx->setCursor(cx1 - 38, cy0 + 22);
  gfx->print("...");

  const int gx0 = cx0 + 10, gx1 = cx1 - 48;
  const int gy0 = cy0 + 30, gy1 = cy1 - 24;
  float vmin = cfg.use_mmol ? 2.0f : 36.0f, vmax = cfg.use_mmol ? 22.0f : 396.0f;
  auto yfor = [&](float v) {
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return gy1 - (int)((v - vmin) / (vmax - vmin) * (gy1 - gy0));
  };
  // target band + threshold lines + right-axis labels
  int yl = yfor(cfg.low_mmol * (cfg.use_mmol ? 1.0f : 18.016f));
  int yh = yfor(cfg.high_mmol * (cfg.use_mmol ? 1.0f : 18.016f));
  gfx->fillRect(gx0, yh, gx1 - gx0, yl - yh, DX_BAND);
  gfx->drawFastHLine(gx0, yh, gx1 - gx0, DX_YEL);
  gfx->drawFastHLine(gx0, yl, gx1 - gx0, DX_RED);
  font_px(1);
  char lb[8];
  gfx->setTextColor(DX_GRAY, DX_CARD);
  snprintf(lb, sizeof(lb), "%.0f", cfg.use_mmol ? 22.0f : 396.0f);
  gfx->setCursor(gx1 + 6, yfor(cfg.use_mmol ? 22.0f : 396.0f) + 2); gfx->print(lb);
  gfx->setTextColor(DX_YEL, DX_CARD);
  snprintf(lb, sizeof(lb), "%.1f", cfg.high_mmol);
  gfx->setCursor(gx1 + 6, yh - 3); gfx->print(lb);
  gfx->setTextColor(DX_RED, DX_CARD);
  snprintf(lb, sizeof(lb), "%.1f", cfg.low_mmol);
  gfx->setCursor(gx1 + 6, yl - 3); gfx->print(lb);

  // hour ticks + "Nu"
  time_t t0 = now - 3 * 3600;
  gfx->setTextColor(DX_GRAY, DX_CARD);
  for (time_t ht = ((t0 / 3600) + 1) * 3600; ht < now; ht += 3600) {
    int x = gx0 + (int)((float)(ht - t0) / (3 * 3600) * (gx1 - gx0));
    struct tm tmh;
    localtime_r(&ht, &tmh);
    snprintf(lb, sizeof(lb), "%d", tmh.tm_hour);
    gfx->setCursor(x - 3, gy1 + 8);
    gfx->print(lb);
  }
  gfx->setCursor(gx1 - 12, gy1 + 8);
  gfx->print("Nu");

  // the dots, black like the app's
  for (int i = hist_n - 1; i >= 0; i--) {
    if (hist[i].t < t0) continue;
    int x = gx0 + (int)((float)(hist[i].t - t0) / (3 * 3600) * (gx1 - gx0));
    int y = yfor(shown_value(hist[i].mgdl) * (cfg.use_mmol ? 1.0f : 1.0f));
    gfx->fillCircle(x, y, 2, DX_TEXT);
  }
  // ring the newest reading, like the app's open circle
  if (hist_n > 0 && hist[0].t >= t0) {
    int x = gx0 + (int)((float)(hist[0].t - t0) / (3 * 3600) * (gx1 - gx0));
    int y = yfor(shown_value(hist[0].mgdl));
    gfx->fillCircle(x, y, 4, DX_CARD);
    gfx->drawCircle(x, y, 4, DX_TEXT);
    gfx->drawCircle(x, y, 3, DX_TEXT);
  }
  gfx->flush();
}

// ---- settings ----------------------------------------------------------------------
static void screen_settings() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("opties");
    char b[32];
    // one column: four value rows with -/+ zones, then toggle/action rows
    struct Row { const char *label; char val[16]; } rows[4];
    snprintf(rows[0].val, 16, "%s", cfg.use_mmol ? "mmol/L" : "mg/dL"); rows[0].label = "eenheid";
    snprintf(rows[1].val, 16, "%.1f", cfg.low_mmol);   rows[1].label = "laag";
    snprintf(rows[2].val, 16, "%.1f", cfg.high_mmol);  rows[2].label = "hoog";
    snprintf(rows[3].val, 16, "%u%%", cfg.volume);     rows[3].label = "volume";
    for (int i = 0; i < 4; i++) {
      int y = 56 + i * 47;
      gfx->fillRoundRect(4, y, 224, 43, 8, TH_PANEL);
      font_med();
      gfx->setTextColor(TH_DIM);
      gfx->setCursor(14, y + 29); gfx->print(rows[i].label);
      gfx->setTextColor(TH_TEXT);
      gfx->setCursor(120, y + 29); gfx->print(rows[i].val);
      btn(232, y, 40, 43, "-", false);
      btn(276, y, 40, 43, "+", false);
    }
    snprintf(b, sizeof(b), "alarmen: %s", cfg.alarms_on ? "aan" : "uit");
    btn(4, 56 + 4 * 47, 312, 43, b, cfg.alarms_on);
    btn(4, 56 + 5 * 47, 312, 43,
        cfg.night_dim ? "nachtdim: aan (22-07)" : "nachtdim: uit", false);
    btn(4, 56 + 6 * 47, 312, 43, "dexcom-login wijzigen", false);
    btn(4, 56 + 7 * 47, 312, 43, "wifi wijzigen", false);
    btn(4, 56 + 8 * 47, 312, 43, "terug", true);
    gfx->flush();
    wait_tap();
    beep_click(cfg.volume);
    bool minus = false;
    int row = -1;
    for (int i = 0; i < 4; i++) {
      int y = 56 + i * 47;
      if (in(232, y, 40, 43)) { row = i; minus = true; }
      if (in(276, y, 40, 43)) { row = i; minus = false; }
    }
    if (row == 0) cfg.use_mmol = !cfg.use_mmol;
    else if (row == 1) cfg.low_mmol = constrain(cfg.low_mmol + (minus ? -0.5f : 0.5f), 3.0f, 6.5f);
    else if (row == 2) cfg.high_mmol = constrain(cfg.high_mmol + (minus ? -0.5f : 0.5f), 7.0f, 20.0f);
    else if (row == 3) {
      cfg.volume = constrain((int)cfg.volume + (minus ? -10 : 10), 10, 100);
      beep_ok(cfg.volume);
    }
    else if (in(4, 56 + 4 * 47, 312, 43)) cfg.alarms_on = !cfg.alarms_on;
    else if (in(4, 56 + 5 * 47, 312, 43)) cfg.night_dim = !cfg.night_dim;
    else if (in(4, 56 + 6 * 47, 312, 43)) { config_save(); screen_dex_login(); }
    else if (in(4, 56 + 7 * 47, 312, 43)) { config_save(); screen_wifi_setup(); }
    else if (in(4, 56 + 8 * 47, 312, 43)) { config_save(); return; }
    config_save();
  }
}

// ---- alarms ---------------------------------------------------------------------------
static void handle_alarms() {
  if (!cfg.alarms_on || hist_n == 0) return;
  if (millis() < snooze_until_ms) return;
  const Egv &cur = hist[0];
  if (time(nullptr) - cur.t > 12 * 60) return;    // stale data never alarms
  float v = to_mmol(cur.mgdl);
  if (cur.t == last_alarm_t) return;              // one alarm per reading max
  if (v < cfg.low_mmol) {
    beep_alarm_low(cfg.volume);
    last_alarm_t = cur.t;
  } else if (v > cfg.high_mmol) {
    beep_alarm_high(cfg.volume);
    last_alarm_t = cur.t;
  }
}

// ---- setup / loop ----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== AiMELO glucose kiosk ===");
  config_load();
  if (!display_begin()) Serial.println("display init failed");
  touch_begin();
  beeps_begin();

  screen_disclaimer();

  WiFi.mode(WIFI_STA);
  if (cfg.wifi_ssid[0] == '\0' || !wifi_connect_ui(cfg.wifi_ssid, cfg.wifi_pass))
    screen_wifi_setup();
  net_ok = (WiFi.status() == WL_CONNECTED);
  Serial.printf("wifi: %s (%s)\n", cfg.wifi_ssid, WiFi.localIP().toString().c_str());

  // Amsterdam time for the clock and reading ages
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  if (cfg.dex_user[0] == '\0') screen_dex_login();
  dex_ok = true;
  last_fetch_ms = 0;                    // fetch immediately
  draw_main();
}

static uint32_t wake_until_ms = 0;

// True while the newest (non-stale) reading sits outside the target range.
static bool out_of_range_now() {
  if (hist_n == 0) return false;
  if (time(nullptr) - hist[0].t > 12 * 60) return false;
  float v = to_mmol(hist[0].mgdl);
  return v < cfg.low_mmol || v > cfg.high_mmol;
}

void loop() {
  net_ok = (WiFi.status() == WL_CONNECTED);
  if (!net_ok) {
    WiFi.reconnect();
    delay(2000);
  }

  // Backlight: dim at night, but never while out of range, and any tap buys
  // a minute of full brightness.
  {
    time_t nw = time(nullptr);
    struct tm tmnow;
    localtime_r(&nw, &tmnow);
    bool night = cfg.night_dim && (tmnow.tm_hour >= 22 || tmnow.tm_hour < 7);
    bool full = !night || out_of_range_now() || millis() < wake_until_ms;
    display_backlight(full ? 100 : 12);
  }
  if (net_ok && (last_fetch_ms == 0 || millis() - last_fetch_ms > 60000)) {
    int n = dex_fetch(hist, HIST_MAX, 6 * 60);
    if (n > 0) { hist_n = n; dex_ok = true; }
    else if (n < 0) { dex_ok = false; Serial.printf("dexcom: %s\n", dex_last_error); }
    last_fetch_ms = millis();
    draw_main();
    handle_alarms();
  }
  if (millis() - last_draw_ms > 30000) {  // keep clock and 'min geleden' fresh
    draw_main();
    last_draw_ms = millis();
  }
  if (touch_tapped()) {
    if (in(266, 286, 50, 44)) {        // the "..." on the graph card
      beep_click(cfg.volume);
      screen_settings();
      touch_irq = false;
      last_fetch_ms = 0;               // settings may have changed server/units
    } else {
      // any other tap: wake the screen; snooze alarms only when one is active
      wake_until_ms = millis() + 60000;
      if (out_of_range_now())
        snooze_until_ms = millis() + 30UL * 60UL * 1000UL;
      beep_click(cfg.volume);
    }
    draw_main();
  }
  delay(20);
}
