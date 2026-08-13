// Glucose display for the Guition JC3248W535C: a native Dexcom Share client.
// Dexcom-app-style value circle with trend arrow, adjustable-window graph,
// day statistics, speaker alarms (low / high / fast-drop / no-data), night
// dim or amber night clock, morning greeting, on-screen WiFi and account
// setup, a read-only status page over HTTP, all settings in NVS.
//
// NOT A MEDICAL DEVICE. Readings arrive via Dexcom Share with the usual
// 5-minute cadence; treatment decisions belong on the official Dexcom app.

#define FW_VERSION "1.6"

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "config.h"
#include "display.h"
#include "touch.h"
#include "beeps.h"
#include "keyboard2.h"
#include "dexcom.h"
#include "battery.h"

// ---- state ------------------------------------------------------------------
#define HIST_MAX 288                   // 24 hours of 5-minute readings
static Egv hist[HIST_MAX];
static int hist_n = 0;
static uint32_t last_fetch_ms = 0;
static uint32_t last_draw_ms = 0;
static time_t last_alarm_t = 0;        // reading timestamp we last alarmed on
static uint32_t last_nodata_ms = 0;
static uint32_t snooze_until_ms = 0;
static uint32_t wake_until_ms = 0;
static int greeted_yday = -1;          // tm_yday of the last morning greeting
static uint8_t alarm_kind = 0;         // 0 none, 1 low, 2 high (active episode)
static uint8_t alarm_count = 0;        // sounds played this episode
static uint32_t alarm_next_ms = 0;
static bool was_out = false;
static bool net_ok = false, dex_ok = false;
static WebServer web(80);

static float to_mmol(int mgdl) { return mgdl / 18.016f; }
static float shown_value(int mgdl) { return cfg.use_mmol ? to_mmol(mgdl) : (float)mgdl; }

static bool is_night() {
  time_t nw = time(nullptr);
  struct tm tmnow;
  localtime_r(&nw, &tmnow);
  int h = tmnow.tm_hour, s = cfg.night_start, e = cfg.night_end;
  if (s == e) return false;
  return s < e ? (h >= s && h < e) : (h >= s || h < e);
}

// True while the newest (non-stale) reading sits outside the target range.
static bool out_of_range_now() {
  if (hist_n == 0) return false;
  if (time(nullptr) - hist[0].t > 12 * 60) return false;
  float v = to_mmol(hist[0].mgdl);
  return v < cfg.low_mmol || v > cfg.high_mmol;
}

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
static void wait_tap() {
  touch_irq = false;
  for (;;) { delay(20); if (touch_tapped()) return; }
}
// Like wait_tap but gives up after ms; returns true when tapped.
static bool wait_tap_for(uint32_t ms) {
  touch_irq = false;
  uint32_t until = millis() + ms;
  while (millis() < until) {
    delay(20);
    if (touch_tapped()) return true;
  }
  return false;
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
    // A scan fails (negative code) while the station is still retrying a
    // stored network. Stop it, settle, then scan.
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(200);
    WiFi.scanDelete();
    int n = WiFi.scanNetworks();
    if (n < 0) n = 0;
    gfx->fillScreen(TH_BG);
    display_header("wifi");
    font_med();
    gfx->setTextColor(TH_DIM);
    gfx->setCursor(MARGIN, 78); gfx->print("kies je netwerk:");
    int shown = min(n, 8);
    if (shown == 0)
      display_centred("geen netwerken gevonden", 200, 2, GL_LOW);
    for (int i = 0; i < shown; i++) {
      int y = 90 + i * 42;
      gfx->fillRoundRect(4, y, 312, 36, 6, TH_PANEL);
      font_med();
      gfx->setTextColor(TH_TEXT);
      gfx->setCursor(14, y + 27);
      char nm[19];
      strncpy(nm, WiFi.SSID(i).c_str(), 18); nm[18] = '\0';
      gfx->print(nm);
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

// ---- trend arrow ------------------------------------------------------------------
static void draw_trend_arrow(int cx, int cy, int8_t trend, uint16_t color) {
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

// ---- main screen -----------------------------------------------------------------
static void draw_main() {
  gfx->fillScreen(DX_BG);
  time_t now = time(nullptr);

  // the owner's name, script + heart, top-left; battery top-right
  draw_name(12, 44, DX_PINK, DX_PINKD);
  battery_draw(SCR_W - 42, 12, DX_GRAY, DX_RED);
  if (!net_ok) {
    font_small();
    gfx->setTextColor(DX_RED);
    gfx->setCursor(SCR_W - text_w("geen wifi") - 6, 48);
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

  draw_trend_arrow(ccx + R + 42, ccy, stale ? -1 : cur.trend, DX_TEXT);

  char sub[32];
  snprintf(sub, sizeof(sub), "%d min geleden", age_min);
  if (stale) { font_med(); gfx->setTextColor(DX_RED); }
  else { font_small(); gfx->setTextColor(DX_GRAY); }
  gfx->setCursor((SCR_W - text_w(sub)) / 2, 278);
  gfx->print(sub);

  // graph card, window per cfg.graph_hours; tap it for the day statistics
  const int cx0 = 8, cy0 = 288, cx1 = SCR_W - 8, cy1 = 472;
  gfx->fillRoundRect(cx0, cy0, cx1 - cx0, cy1 - cy0, 12, DX_CARD);
  font_med();
  gfx->setTextColor(DX_GRAY);
  gfx->setCursor(cx1 - 38, cy0 + 22);
  gfx->print("...");
  font_small();
  gfx->setTextColor(DX_GRAY);
  char wt[8];
  snprintf(wt, sizeof(wt), "%uu", cfg.graph_hours);
  gfx->setCursor(cx0 + 10, cy0 + 20);
  gfx->print(wt);

  const int gx0 = cx0 + 10, gx1 = cx1 - 48;
  const int gy0 = cy0 + 30, gy1 = cy1 - 24;
  const int win_s = cfg.graph_hours * 3600;
  float vmin = cfg.use_mmol ? 2.0f : 36.0f, vmax = cfg.use_mmol ? 22.0f : 396.0f;
  auto yfor = [&](float v) {
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    return gy1 - (int)((v - vmin) / (vmax - vmin) * (gy1 - gy0));
  };
  int yl = yfor(cfg.low_mmol * (cfg.use_mmol ? 1.0f : 18.016f));
  int yh = yfor(cfg.high_mmol * (cfg.use_mmol ? 1.0f : 18.016f));
  gfx->fillRect(gx0, yh, gx1 - gx0, yl - yh, DX_BAND);
  gfx->drawFastHLine(gx0, yh, gx1 - gx0, DX_YEL);
  gfx->drawFastHLine(gx0, yl, gx1 - gx0, DX_RED);
  font_px(1);
  char lb[8];
  gfx->setTextColor(DX_YEL, DX_CARD);
  snprintf(lb, sizeof(lb), "%.1f", cfg.high_mmol);
  gfx->setCursor(gx1 + 6, yh - 3); gfx->print(lb);
  gfx->setTextColor(DX_RED, DX_CARD);
  snprintf(lb, sizeof(lb), "%.1f", cfg.low_mmol);
  gfx->setCursor(gx1 + 6, yl - 3); gfx->print(lb);

  // hour ticks + "Nu" (tick spacing grows with the window)
  time_t t0 = now - win_s;
  int tick_h = cfg.graph_hours <= 6 ? 1 : (cfg.graph_hours == 12 ? 3 : 6);
  gfx->setTextColor(DX_GRAY, DX_CARD);
  for (time_t ht = ((t0 / 3600) + 1) * 3600; ht < now; ht += 3600) {
    struct tm tmh;
    localtime_r(&ht, &tmh);
    if (tmh.tm_hour % tick_h) continue;
    int x = gx0 + (int)((float)(ht - t0) / win_s * (gx1 - gx0));
    if (x > gx1 - 20) continue;
    snprintf(lb, sizeof(lb), "%d", tmh.tm_hour);
    gfx->setCursor(x - 3, gy1 + 8);
    gfx->print(lb);
  }
  gfx->setCursor(gx1 - 12, gy1 + 8);
  gfx->print("Nu");

  for (int i = hist_n - 1; i >= 0; i--) {
    if (hist[i].t < t0) continue;
    int x = gx0 + (int)((float)(hist[i].t - t0) / win_s * (gx1 - gx0));
    int y = yfor(shown_value(hist[i].mgdl));
    gfx->fillCircle(x, y, 2, DX_TEXT);
  }
  if (hist[0].t >= t0) {
    int x = gx0 + (int)((float)(hist[0].t - t0) / win_s * (gx1 - gx0));
    int y = yfor(shown_value(hist[0].mgdl));
    gfx->fillCircle(x, y, 4, DX_CARD);
    gfx->drawCircle(x, y, 4, DX_TEXT);
    gfx->drawCircle(x, y, 3, DX_TEXT);
  }
  gfx->flush();
}

// ---- night clock -------------------------------------------------------------------
// Amber on black, nothing else: a bedside clock that happens to know glucose.
static void draw_night() {
  gfx->fillScreen(0x0000);
  if (hist_n == 0) {
    font_med();
    gfx->setTextColor(NT_AMBER);
    gfx->setCursor((SCR_W - text_w("geen data")) / 2, 240);
    gfx->print("geen data");
    gfx->flush();
    return;
  }
  time_t now = time(nullptr);
  const Egv &cur = hist[0];
  bool stale = (now - cur.t) > 12 * 60;
  char val[12];
  if (cfg.use_mmol) snprintf(val, sizeof(val), "%.1f", to_mmol(cur.mgdl));
  else snprintf(val, sizeof(val), "%d", cur.mgdl);
  font_big();
  gfx->setTextColor(NT_AMBER);
  gfx->setCursor((SCR_W - text_w(val)) / 2, 220);
  gfx->print(val);
  draw_trend_arrow(SCR_W / 2, 290, stale ? -1 : cur.trend, NT_AMBER);
  if (stale) {
    font_small();
    gfx->setTextColor(DX_RED);
    gfx->setCursor((SCR_W - text_w("data is verouderd")) / 2, 350);
    gfx->print("data is verouderd");
  }
  gfx->flush();
}

// ---- day statistics ------------------------------------------------------------------
static void screen_stats() {
  for (;;) {
    time_t now = time(nullptr);
    time_t t24 = now - 24 * 3600;
    int n = 0, nlow = 0, nhigh = 0, low_events = 0;
    float sum = 0, mn = 1e9f, mx = -1e9f;
    bool was_low = false;
    for (int i = hist_n - 1; i >= 0; i--) {   // oldest -> newest
      if (hist[i].t < t24) continue;
      float v = to_mmol(hist[i].mgdl);
      n++; sum += v;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
      bool lo = v < cfg.low_mmol;
      if (lo && !was_low) low_events++;
      was_low = lo;
      if (lo) nlow++;
      else if (v > cfg.high_mmol) nhigh++;
    }
    gfx->fillScreen(DX_BG);
    draw_name(12, 44, DX_PINK, DX_PINKD);
    font_med();
    gfx->setTextColor(DX_GRAY);
    gfx->setCursor(SCR_W - text_w("24 uur") - 8, 36);
    gfx->print("24 uur");

    if (n == 0) {
      display_centred("nog geen data", 200, 2, DX_GRAY);
    } else {
      int pin = 100 * (n - nlow - nhigh) / n;
      int plow = 100 * nlow / n;
      int phigh = 100 - pin - plow;
      // TIR bar: low red | in-range green | high yellow
      const int bx = 16, bw = SCR_W - 32, by = 76, bh = 34;
      int wlow = bw * plow / 100, whigh = bw * phigh / 100;
      int win_ = bw - wlow - whigh;
      gfx->fillRoundRect(bx, by, bw, bh, 8, DX_BAND);
      if (wlow) gfx->fillRect(bx, by, wlow, bh, DX_RED);
      if (win_) gfx->fillRect(bx + wlow, by, win_, bh, DX_GREEN);
      if (whigh) gfx->fillRect(bx + wlow + win_, by, whigh, bh, DX_YEL);
      font_small();
      gfx->setTextColor(DX_GRAY);
      gfx->setCursor(bx, by + bh + 20); gfx->print("laag");
      gfx->setCursor(bx + bw / 2 - 20, by + bh + 20); gfx->print("in bereik");
      gfx->setCursor(bx + bw - 30, by + bh + 20); gfx->print("hoog");
      char pct[8];
      font_med();
      gfx->setTextColor(DX_TEXT);
      snprintf(pct, sizeof(pct), "%d%%", plow);
      gfx->setCursor(bx, by + bh + 48); gfx->print(pct);
      snprintf(pct, sizeof(pct), "%d%%", pin);
      gfx->setCursor(bx + bw / 2 - 20, by + bh + 48); gfx->print(pct);
      snprintf(pct, sizeof(pct), "%d%%", phigh);
      gfx->setCursor(bx + bw - 40, by + bh + 48); gfx->print(pct);

      // stat rows
      struct { const char *label; float v; } st[3] = {
        { "gemiddeld", sum / n }, { "laagste", mn }, { "hoogste", mx },
      };
      for (int i = 0; i < 3; i++) {
        int y = 190 + i * 52;
        gfx->fillRoundRect(16, y, SCR_W - 32, 44, 10, DX_CARD);
        font_med();
        gfx->setTextColor(DX_GRAY);
        gfx->setCursor(28, y + 30); gfx->print(st[i].label);
        char v[16];
        snprintf(v, sizeof(v), cfg.use_mmol ? "%.1f" : "%.0f",
                 cfg.use_mmol ? st[i].v : st[i].v * 18.016f);
        gfx->setTextColor(DX_TEXT);
        gfx->setCursor(SCR_W - 28 - text_w(v), y + 30); gfx->print(v);
      }
      char le[36];
      snprintf(le, sizeof(le), "%d keer laag geweest", low_events);
      display_centred(le, 352, 1, low_events ? DX_RED : DX_GRAY);
    }

    char wb[24];
    snprintf(wb, sizeof(wb), "grafiek: %u uur", cfg.graph_hours);
    // dark buttons read fine on the light page too
    btn(16, 396, 180, 44, wb, false);
    btn(220, 396, 84, 44, "terug", true);
    gfx->flush();
    wait_tap();
    beep_click(cfg.volume);
    if (in(16, 396, 180, 44)) {
      cfg.graph_hours = cfg.graph_hours == 3 ? 6 :
                        cfg.graph_hours == 6 ? 12 :
                        cfg.graph_hours == 12 ? 24 : 3;
      config_save();
      continue;
    }
    return;
  }
}

// ---- morning greeting ------------------------------------------------------------------
static void screen_greeting() {
  time_t now = time(nullptr);
  float mn = 1e9f, mx = -1e9f;
  struct tm tmn;
  localtime_r(&now, &tmn);
  struct tm mid = tmn;
  mid.tm_hour = 0; mid.tm_min = 0; mid.tm_sec = 0;
  time_t midnight = mktime(&mid);
  for (int i = 0; i < hist_n; i++) {
    if (hist[i].t < midnight) continue;
    float v = to_mmol(hist[i].mgdl);
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  gfx->fillScreen(DX_BG);
  font_med();
  gfx->setTextColor(DX_GRAY);
  gfx->setCursor((SCR_W - text_w("Goedemorgen")) / 2, 140);
  gfx->print("Goedemorgen");
  font_script();
  gfx->setTextColor(DX_PINK);
  gfx->setCursor((SCR_W - text_w(cfg.display_name)) / 2 - 10, 210);
  gfx->print(cfg.display_name);
  draw_heart(SCR_W / 2 + text_w(cfg.display_name) / 2 + 12, 198, 8, DX_PINKD);
  if (mx > 0) {
    display_centred("vannacht tussen", 248, 2, DX_TEXT);
    char s[24];
    snprintf(s, sizeof(s), "%.1f en %.1f", mn, mx);
    display_centred(s, 284, 2, DX_TEXT);
  }
  display_centred("fijne dag!", 330, 1, DX_GRAY);
  gfx->flush();
  wait_tap_for(30000);
  touch_irq = false;
}

// ---- credits ------------------------------------------------------------------------------
static void screen_credits() {
  gfx->fillScreen(TH_BG);
  draw_name((SCR_W - text_w(cfg.display_name)) / 2 - 10, 70, TH_ACCENT, TH_ACCENT);
  display_centred("glucose-display", 92, 2, TH_TEXT);
  char v[24];
  snprintf(v, sizeof(v), "versie %s", FW_VERSION);
  display_centred(v, 122, 1, TH_DIM);
  font_small();
  gfx->setTextColor(TH_DIM);
  int y = 180;
  const char *lines[] = {
    "met liefde gebouwd door",
    "Dimitri & Claude",
    "",
    "data: Dexcom Share",
    "board: JC3248W535C (ESP32-S3)",
    "",
    "geen medisch hulpmiddel",
  };
  for (auto l : lines) {
    gfx->setTextColor(strcmp(l, "Dimitri & Claude") == 0 ? TH_TEXT : TH_DIM);
    gfx->setCursor((SCR_W - text_w(l)) / 2, y);
    gfx->print(l);
    y += 26;
  }
  if (bat_present && bat_pct >= 0) {
    char bl[48];
    snprintf(bl, sizeof(bl), "accu: %d%% (%.2fV)%s", bat_pct, bat_volt,
             bat_full ? " vol" : (bat_charging ? " opladen" : ""));
    display_centred(bl, 388, 1, TH_DIM);
  }
  char ip[48];
  snprintf(ip, sizeof(ip), "op je telefoon: http://%s", WiFi.localIP().toString().c_str());
  display_centred(ip, 358, 1, TH_ACCENT);
  snprintf(ip, sizeof(ip), "web-login: display / %s", cfg.web_pass);
  display_centred(ip, 382, 1, TH_TEXT);
  btn(60, 410, 200, 48, "terug", true);
  gfx->flush();
  wait_tap();
  beep_click(cfg.volume);
}

// ---- test menu -----------------------------------------------------------------------
// Hear every sound and preview the special screens without waiting for the
// real thing to happen.
static void screen_test() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("testen");
    btn(4, 62, 152, 52, "klik", false);
    btn(164, 62, 152, 52, "chime", false);
    btn(4, 122, 152, 52, "laag alarm", false);
    btn(164, 122, 152, 52, "hoog alarm", false);
    btn(4, 182, 152, 52, "snel-dalend", false);
    btn(164, 182, 152, 52, "geen-data", false);
    btn(4, 258, 312, 48, "nachtklok bekijken", false);
    btn(4, 314, 312, 48, "goedemorgen bekijken", false);
    btn(4, 412, 312, 48, "terug", true);
    gfx->flush();
    wait_tap();
    if (in(4, 62, 152, 52)) beep_click(cfg.volume);
    else if (in(164, 62, 152, 52)) beep_ok(cfg.volume);
    else if (in(4, 122, 152, 52)) beep_alarm_low(cfg.volume);
    else if (in(164, 122, 152, 52)) beep_alarm_high(cfg.volume);
    else if (in(4, 182, 152, 52)) beep_fastdrop(cfg.volume);
    else if (in(164, 182, 152, 52)) beep_nodata(cfg.volume);
    else if (in(4, 258, 312, 48)) {
      beep_click(cfg.volume);
      draw_night();
      display_backlight(cfg.dim_pct);   // exactly as it looks at night
      wait_tap();
      display_backlight(100);
    }
    else if (in(4, 314, 312, 48)) { beep_click(cfg.volume); screen_greeting(); }
    else if (in(4, 412, 312, 48)) { beep_click(cfg.volume); return; }
  }
}

// Generate a fresh web password: short, unambiguous, shown only on the
// device's credits screen.
static void web_pass_generate() {
  static const char CS[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 6; i++)
    cfg.web_pass[i] = CS[esp_random() % (sizeof(CS) - 1)];
  cfg.web_pass[6] = '\0';
  config_save();
}

// ---- settings (two pages) -------------------------------------------------------------------
// Page 3: account, tests and the web password.
static void screen_settings_system() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("systeem");
    btn(4, 62, 312, 46, "naam wijzigen", false);
    btn(4, 116, 312, 46, "dexcom-login wijzigen", false);
    btn(4, 170, 312, 46, "wifi wijzigen", false);
    btn(4, 224, 312, 46, "testen...", false);
    btn(4, 278, 312, 46, "web-wachtwoord vernieuwen", false);
    btn(4, 332, 312, 46, "over dit display", false);
    btn(4, 420, 312, 46, "terug", true);
    gfx->flush();
    wait_tap();
    beep_click(cfg.volume);
    if (in(4, 62, 312, 46)) {
      kb_input("naam op het display:", cfg.display_name, sizeof(cfg.display_name), false);
      if (cfg.display_name[0] == '\0') strcpy(cfg.display_name, "Chantie");
    }
    else if (in(4, 116, 312, 46)) { config_save(); screen_dex_login(); }
    else if (in(4, 170, 312, 46)) { config_save(); screen_wifi_setup(); }
    else if (in(4, 224, 312, 46)) screen_test();
    else if (in(4, 278, 312, 46)) { web_pass_generate(); screen_credits(); }
    else if (in(4, 332, 312, 46)) screen_credits();
    else if (in(4, 420, 312, 46)) { config_save(); return; }
    config_save();
  }
}

static void screen_settings_more() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("meer");
    char b[40];
    snprintf(b, sizeof(b), "snel-dalend alarm: %s", cfg.alarm_fast ? "aan" : "uit");
    btn(4, 60, 312, 42, b, cfg.alarm_fast);
    snprintf(b, sizeof(b), "geen-data alarm: %s", cfg.alarm_nodata ? "aan" : "uit");
    btn(4, 108, 312, 42, b, cfg.alarm_nodata);
    snprintf(b, sizeof(b), "hoog stil 's nachts: %s", cfg.quiet_high_night ? "aan" : "uit");
    btn(4, 156, 312, 42, b, cfg.quiet_high_night);
    static const char *ASTYLES[3] = {"zacht", "klassiek", "fel"};
    snprintf(b, sizeof(b), "alarmgeluid: %s", ASTYLES[cfg.alarm_style % 3]);
    btn(4, 204, 312, 42, b, false);
    // night window hours with -/+
    snprintf(b, sizeof(b), "herhaal alarm %ux", cfg.alarm_repeats);
    gfx->fillRoundRect(4, 250, 224, 40, 8, TH_PANEL);
    font_med(); gfx->setTextColor(TH_TEXT);
    gfx->setCursor(14, 250 + 28); gfx->print(b);
    btn(232, 250, 40, 40, "-", false);
    btn(276, 250, 40, 40, "+", false);
    snprintf(b, sizeof(b), "nacht start  %02u:00", cfg.night_start);
    gfx->fillRoundRect(4, 296, 224, 40, 8, TH_PANEL);
    gfx->setTextColor(TH_TEXT);
    gfx->setCursor(14, 296 + 28); gfx->print(b);
    btn(232, 296, 40, 40, "-", false);
    btn(276, 296, 40, 40, "+", false);
    snprintf(b, sizeof(b), "nacht eind   %02u:00", cfg.night_end);
    gfx->fillRoundRect(4, 342, 224, 40, 8, TH_PANEL);
    gfx->setTextColor(TH_TEXT);
    gfx->setCursor(14, 342 + 28); gfx->print(b);
    btn(232, 342, 40, 40, "-", false);
    btn(276, 342, 40, 40, "+", false);
    btn(4, 392, 312, 40, "systeem & testen...", false);
    btn(4, 438, 312, 40, "terug", true);
    gfx->flush();
    wait_tap();
    beep_click(cfg.volume);
    if (in(4, 60, 312, 42)) cfg.alarm_fast = !cfg.alarm_fast;
    else if (in(4, 108, 312, 42)) cfg.alarm_nodata = !cfg.alarm_nodata;
    else if (in(4, 156, 312, 42)) cfg.quiet_high_night = !cfg.quiet_high_night;
    else if (in(4, 204, 312, 42)) {
      cfg.alarm_style = (cfg.alarm_style + 1) % 3;
      beep_alarm_high(cfg.volume);            // instant preview
    }
    else if (in(232, 250, 40, 40)) cfg.alarm_repeats = max(1, cfg.alarm_repeats - 1);
    else if (in(276, 250, 40, 40)) cfg.alarm_repeats = min(10, cfg.alarm_repeats + 1);
    else if (in(232, 296, 40, 40)) cfg.night_start = (cfg.night_start + 23) % 24;
    else if (in(276, 296, 40, 40)) cfg.night_start = (cfg.night_start + 1) % 24;
    else if (in(232, 342, 40, 40)) cfg.night_end = (cfg.night_end + 23) % 24;
    else if (in(276, 342, 40, 40)) cfg.night_end = (cfg.night_end + 1) % 24;
    else if (in(4, 392, 312, 40)) { config_save(); screen_settings_system(); }
    else if (in(4, 438, 312, 40)) { config_save(); return; }
    config_save();
  }
}

static void screen_settings() {
  for (;;) {
    gfx->fillScreen(TH_BG);
    display_header("opties");
    char b[36];
    struct Row { const char *label; char val[16]; } rows[6];
    snprintf(rows[0].val, 16, "%s", cfg.use_mmol ? "mmol/L" : "mg/dL"); rows[0].label = "eenheid";
    snprintf(rows[1].val, 16, "%.1f", cfg.low_mmol);   rows[1].label = "laag";
    snprintf(rows[2].val, 16, "%.1f", cfg.high_mmol);  rows[2].label = "hoog";
    snprintf(rows[3].val, 16, "%u%%", cfg.volume);     rows[3].label = "volume";
    snprintf(rows[4].val, 16, "%u%%", cfg.dim_pct);    rows[4].label = "dim";
    snprintf(rows[5].val, 16, "%s", th_name());        rows[5].label = "kleur";
    for (int i = 0; i < 6; i++) {
      int y = 54 + i * 40;
      gfx->fillRoundRect(4, y, 224, 36, 8, TH_PANEL);
      font_med();
      gfx->setTextColor(TH_DIM);
      gfx->setCursor(14, y + 26); gfx->print(rows[i].label);
      gfx->setTextColor(i == 5 ? TH_ACCENT : TH_TEXT);
      gfx->setCursor(120, y + 26); gfx->print(rows[i].val);
      btn(232, y, 40, 36, "-", false);
      btn(276, y, 40, 36, "+", false);
    }
    snprintf(b, sizeof(b), "alarmen: %s", cfg.alarms_on ? "aan" : "uit");
    btn(4, 54 + 6 * 40, 312, 36, b, cfg.alarms_on);
    char nm[36];
    if (cfg.night_mode == 0) snprintf(nm, sizeof(nm), "nacht: uit");
    else snprintf(nm, sizeof(nm), "nacht: %s (%02u-%02u)",
                  cfg.night_mode == 1 ? "dimmen" : "klok",
                  cfg.night_start, cfg.night_end);
    btn(4, 54 + 7 * 40, 312, 36, nm, cfg.night_mode > 0);
    btn(4, 54 + 8 * 40, 312, 36, "meer opties...", false);
    btn(4, 54 + 9 * 40, 312, 36, "terug", true);
    gfx->flush();
    wait_tap();
    beep_click(cfg.volume);
    bool minus = false;
    int row = -1;
    for (int i = 0; i < 6; i++) {
      int y = 54 + i * 40;
      if (in(232, y, 40, 36)) { row = i; minus = true; }
      if (in(276, y, 40, 36)) { row = i; minus = false; }
    }
    if (row == 0) cfg.use_mmol = !cfg.use_mmol;
    else if (row == 1) cfg.low_mmol = constrain(cfg.low_mmol + (minus ? -0.5f : 0.5f), 3.0f, 6.5f);
    else if (row == 2) cfg.high_mmol = constrain(cfg.high_mmol + (minus ? -0.5f : 0.5f), 7.0f, 20.0f);
    else if (row == 3) {
      cfg.volume = constrain((int)cfg.volume + (minus ? -10 : 10), 10, 100);
      beep_ok(cfg.volume);
    }
    else if (row == 4) {
      cfg.dim_pct = constrain((int)cfg.dim_pct + (minus ? -5 : 5), 5, 60);
      display_backlight(cfg.dim_pct);   // live preview
    }
    else if (row == 5)
      cfg.theme = (cfg.theme + (minus ? THEME_COUNT - 1 : 1)) % THEME_COUNT;
    else if (in(4, 54 + 6 * 40, 312, 36)) cfg.alarms_on = !cfg.alarms_on;
    else if (in(4, 54 + 7 * 40, 312, 36)) cfg.night_mode = (cfg.night_mode + 1) % 3;
    else if (in(4, 54 + 8 * 40, 312, 36)) { config_save(); screen_settings_more(); }
    else if (in(4, 54 + 9 * 40, 312, 36)) { config_save(); display_backlight(100); return; }
    if (row != 4) display_backlight(100);
    config_save();
  }
}

// ---- alarms ---------------------------------------------------------------------------
// Out-of-range alarms are EPISODES: they start when a reading crosses out of
// range, re-sound every 30 seconds up to cfg.alarm_repeats times, stop on a
// tap (30-minute snooze), and re-arm only after the value has been back in
// range. Fast-drop stays a one-shot advisory; no-data has its own cadence.
static void handle_alarms() {
  if (!cfg.alarms_on || hist_n == 0) return;
  time_t now = time(nullptr);
  const Egv &cur = hist[0];

  // no data coming in: a soft nudge every 15 minutes (independent of snooze:
  // a snoozed LOW should not silence a broken sensor)
  if (cfg.alarm_nodata && now - cur.t > 20 * 60) {
    if (millis() - last_nodata_ms > 15UL * 60000UL) {
      beep_nodata(cfg.volume);
      last_nodata_ms = millis();
    }
    return;                              // stale data: no value alarms
  }
  if (now - cur.t > 12 * 60) return;

  float v = to_mmol(cur.mgdl);
  float prev = hist_n > 1 ? to_mmol(hist[1].mgdl) : v;
  bool low = v < cfg.low_mmol, high = v > cfg.high_mmol;

  if (low || high) {
    if (!was_out) {                      // a fresh episode begins
      was_out = true;
      alarm_kind = low ? 1 : 2;
      alarm_count = 0;
      alarm_next_ms = millis();          // first sound right away
    } else if ((low && alarm_kind == 2) || (high && alarm_kind == 1)) {
      alarm_kind = low ? 1 : 2;          // flipped side: new episode
      alarm_count = 0;
      alarm_next_ms = millis();
    }
  } else {
    was_out = false;                     // back in range: re-arm
    alarm_kind = 0;
    if (cfg.alarm_fast && millis() >= snooze_until_ms &&
        cur.t != last_alarm_t &&
        (cur.trend == 7 || (prev - v) >= 0.85f) &&
        v < cfg.low_mmol + 2.0f) {
      beep_fastdrop(cfg.volume);         // falling fast toward the floor
      last_alarm_t = cur.t;
    }
  }
}

// Called every loop: plays the active episode's sound on schedule.
static void alarm_pump() {
  if (alarm_kind == 0 || !cfg.alarms_on) return;
  if (millis() < snooze_until_ms) return;          // tapped: silenced
  if (alarm_count >= cfg.alarm_repeats) return;    // gave up for this episode
  if (millis() < alarm_next_ms) return;
  if (alarm_kind == 2 && cfg.quiet_high_night && is_night()) return;
  if (alarm_kind == 1) beep_alarm_low(cfg.volume);
  else beep_alarm_high(cfg.volume);
  alarm_count++;
  alarm_next_ms = millis() + 30000;
}

// ---- web security -------------------------------------------------------------------------
// Everything behind HTTP basic auth (user "display", device-generated
// password, shown only on the credits screen). The Host check blocks DNS
// rebinding; the Origin check on saves blocks cross-site posts. Credentials
// for WiFi/Dexcom are never readable or writable over the web at all.
static bool web_guard() {
  String host = web.hostHeader();
  String ip = WiFi.localIP().toString();
  if (host.length() && !host.startsWith(ip)) {
    web.send(403, "text/plain", "verkeerde host");
    return false;
  }
  if (!web.authenticate("display", cfg.web_pass)) {
    delay(400);                       // slow down guessing
    web.requestAuthentication(BASIC_AUTH, "glucose-display");
    return false;
  }
  return true;
}

static bool web_guard_post() {
  if (!web_guard()) return false;
  String o = web.header("Origin");
  if (o.length() && o.indexOf(WiFi.localIP().toString()) < 0) {
    web.send(403, "text/plain", "verkeerde herkomst");
    return false;
  }
  return true;
}

// ---- web status page --------------------------------------------------------------------
static const char *trend_arrow_txt(int8_t t) {
  switch (t) {
    case 1: return "&uarr;&uarr;"; case 2: return "&uarr;";
    case 3: return "&nearr;"; case 4: return "&rarr;";
    case 5: return "&searr;"; case 6: return "&darr;";
    case 7: return "&darr;&darr;"; default: return "?";
  }
}

static void web_root() {
  if (!web_guard()) return;
  String h = "<!doctype html><html lang='nl'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='60'>"
    "<title>" + String(cfg.display_name) + "</title><style>"
    "body{background:#101820;color:#fff;font-family:system-ui;text-align:center;padding-top:8vh}"
    "h1{color:" + String(th_hex()) + ";font-size:2em;margin:0}"
    ".v{font-size:6em;font-weight:700;margin:.1em 0}.t{font-size:2.5em}"
    ".g{color:#90a0ac}.warn{color:#ff6060}small{color:#90a0ac}</style></head><body>";
  h += "<h1>" + String(cfg.display_name) + " &hearts;</h1>";
  if (hist_n > 0) {
    time_t now = time(nullptr);
    int age = (int)((now - hist[0].t) / 60);
    char val[12];
    if (cfg.use_mmol) snprintf(val, sizeof(val), "%.1f", to_mmol(hist[0].mgdl));
    else snprintf(val, sizeof(val), "%d", hist[0].mgdl);
    float v = to_mmol(hist[0].mgdl);
    const char *col = v < cfg.low_mmol ? "#ff6060" : (v > cfg.high_mmol ? "#ffcc00" : "#fff");
    h += "<div class='v' style='color:" + String(col) + "'>" + val + "</div>";
    h += "<div class='t'>" + String(trend_arrow_txt(hist[0].trend)) + "</div>";
    h += "<p class='" + String(age > 12 ? "warn" : "g") + "'>" + String(age) + " min geleden &middot; "
       + (cfg.use_mmol ? "mmol/L" : "mg/dL") + "</p>";
  } else {
    h += "<p class='g'>nog geen data</p>";
  }
  if (bat_present && bat_pct >= 0) {
    h += "<p class='g'>accu " + String(bat_pct) + "%" +
         (bat_full ? " (vol)" : (bat_charging ? " &#9889; opladen" : "")) + "</p>";
  }
  h += "<p><a href='/instellingen' style='color:" + String(th_hex()) + "'>instellingen</a></p>";
  h += "<small>geen medisch hulpmiddel &middot; ververst elke minuut</small></body></html>";
  web.send(200, "text/html", h);
}

static void web_api() {
  if (!web_guard()) return;
  String j = "{";
  if (hist_n > 0) {
    j += "\"mgdl\":" + String(hist[0].mgdl);
    j += ",\"mmol\":" + String(to_mmol(hist[0].mgdl), 1);
    j += ",\"trend\":\"" + String(hist[0].trend >= 0 ? DEX_TRENDS[hist[0].trend] : "?") + "\"";
    j += ",\"age_min\":" + String((int)((time(nullptr) - hist[0].t) / 60));
  }
  j += "}";
  web.send(200, "application/json", j);
}


// ---- web settings form ---------------------------------------------------------
// Display and alarm preferences only; WiFi and Dexcom credentials can only be
// changed on the device itself, never over the network.
static void render();                    // defined below, used after a save

static void web_settings_form() {
  if (!web_guard()) return;
  String h = "<!doctype html><html lang='nl'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>instellingen</title><style>"
    "body{background:#101820;color:#fff;font-family:system-ui;max-width:26em;margin:0 auto;padding:1em}"
    "h1{color:" + String(th_hex()) + "}label{display:block;margin:.9em 0 .25em;color:#90a0ac}"
    "input,select{width:100%;padding:.5em;border-radius:8px;border:0;background:#33475B;color:#fff;font-size:1em}"
    "input[type=checkbox]{width:auto;transform:scale(1.4);margin-right:.6em}"
    ".row{margin:.6em 0}.ok{color:#7be07b}"
    "button{margin-top:1.2em;width:100%;padding:.8em;border:0;border-radius:10px;"
    "background:" + String(th_hex()) + ";color:#101820;font-size:1.1em;font-weight:700}"
    "a{color:" + String(th_hex()) + "}</style></head><body><h1>instellingen</h1>";
  if (web.hasArg("ok")) h += "<p class='ok'>opgeslagen &#10003;</p>";
  h += "<form method='post' action='/instellingen'>";
  h += "<label>naam op het display</label><input name='name' maxlength='24' value='" + String(cfg.display_name) + "'>";
  h += "<label>eenheid</label><select name='unit'>";
  h += String("<option value='mmol'") + (cfg.use_mmol ? " selected" : "") + ">mmol/L</option>";
  h += String("<option value='mgdl'") + (!cfg.use_mmol ? " selected" : "") + ">mg/dL</option></select>";
  h += "<label>laag alarm (mmol/L)</label><input name='low' type='number' step='0.1' min='3' max='6.5' value='" + String(cfg.low_mmol, 1) + "'>";
  h += "<label>hoog alarm (mmol/L)</label><input name='high' type='number' step='0.1' min='7' max='20' value='" + String(cfg.high_mmol, 1) + "'>";
  h += "<label>volume: " + String(cfg.volume) + "%</label><input name='vol' type='range' min='10' max='100' step='10' value='" + String(cfg.volume) + "'>";
  h += "<label>nachtdim-niveau: " + String(cfg.dim_pct) + "%</label><input name='dim' type='range' min='5' max='60' step='5' value='" + String(cfg.dim_pct) + "'>";
  h += "<label>alarmgeluid</label><select name='astyle'>";
  const char *asty[3] = {"zacht", "klassiek", "fel"};
  for (int i = 0; i < 3; i++)
    h += "<option value='" + String(i) + "'" + (cfg.alarm_style == i ? " selected" : "") + ">" + asty[i] + "</option>";
  h += "</select>";
  h += "<label>alarm herhalen (1-10x)</label><input name='areps' type='number' min='1' max='10' value='" + String(cfg.alarm_repeats) + "'>";
  h += "<label>nacht van (uur)</label><input name='nstart' type='number' min='0' max='23' value='" + String(cfg.night_start) + "'>";
  h += "<label>nacht tot (uur)</label><input name='nend' type='number' min='0' max='23' value='" + String(cfg.night_end) + "'>";
  h += "<label>nachtmodus</label><select name='night'>";
  const char *nopts[3] = {"uit", "dimmen", "amber klok"};
  for (int i = 0; i < 3; i++)
    h += "<option value='" + String(i) + "'" + (cfg.night_mode == i ? " selected" : "") + ">" + nopts[i] + "</option>";
  h += "</select>";
  h += "<label>grafiekvenster</label><select name='ghours'>";
  const int gh[4] = {3, 6, 12, 24};
  for (int i = 0; i < 4; i++)
    h += "<option value='" + String(gh[i]) + "'" + (cfg.graph_hours == gh[i] ? " selected" : "") + ">" + String(gh[i]) + " uur</option>";
  h += "</select>";
  h += "<label>kleur & stijl</label><select name='theme'>";
  for (int i = 0; i < THEME_COUNT; i++)
    h += "<option value='" + String(i) + "'" + (cfg.theme == i ? " selected" : "") + ">"
       + THEME_PRESETS[i].name + (THEME_PRESETS[i].script ? " (sierlijk)" : " (strak)") + "</option>";
  h += "</select>";
  h += String("<div class='row'><label><input type='checkbox' name='alarms'") + (cfg.alarms_on ? " checked" : "") + ">alarmen aan</label></div>";
  h += String("<div class='row'><label><input type='checkbox' name='fast'") + (cfg.alarm_fast ? " checked" : "") + ">snel-dalend alarm</label></div>";
  h += String("<div class='row'><label><input type='checkbox' name='nodata'") + (cfg.alarm_nodata ? " checked" : "") + ">geen-data alarm</label></div>";
  h += String("<div class='row'><label><input type='checkbox' name='qhigh'") + (cfg.quiet_high_night ? " checked" : "") + ">hoog-alarm stil 's nachts</label></div>";
  h += "<button>opslaan</button></form>";
  h += "<p><a href='/'>&larr; terug</a></p>";
  h += "<small style='color:#90a0ac'>wifi en dexcom-login wijzig je op het apparaat zelf</small></body></html>";
  web.send(200, "text/html", h);
}

static void web_settings_save() {
  if (!web_guard_post()) return;
  if (web.hasArg("name")) {
    // keep only characters the script font can draw
    char nm[25]; int k = 0;
    String s = web.arg("name");
    for (unsigned i = 0; i < s.length() && k < 24; i++) {
      char c = s[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ') nm[k++] = c;
    }
    nm[k] = '\0';
    if (nm[0]) strncpy(cfg.display_name, nm, sizeof(cfg.display_name));
  }
  if (web.hasArg("unit")) cfg.use_mmol = (web.arg("unit") == "mmol");
  if (web.hasArg("low"))  cfg.low_mmol  = constrain(web.arg("low").toFloat(), 3.0f, 6.5f);
  if (web.hasArg("high")) cfg.high_mmol = constrain(web.arg("high").toFloat(), 7.0f, 20.0f);
  if (web.hasArg("vol"))  cfg.volume    = constrain(web.arg("vol").toInt(), 10, 100);
  if (web.hasArg("dim"))  cfg.dim_pct   = constrain(web.arg("dim").toInt(), 5, 60);
  if (web.hasArg("night")) cfg.night_mode = constrain(web.arg("night").toInt(), 0, 2);
  if (web.hasArg("astyle")) cfg.alarm_style = constrain(web.arg("astyle").toInt(), 0, 2);
  if (web.hasArg("areps")) cfg.alarm_repeats = constrain(web.arg("areps").toInt(), 1, 10);
  if (web.hasArg("nstart")) cfg.night_start = constrain(web.arg("nstart").toInt(), 0, 23);
  if (web.hasArg("nend"))   cfg.night_end   = constrain(web.arg("nend").toInt(), 0, 23);
  if (web.hasArg("theme")) cfg.theme = constrain(web.arg("theme").toInt(), 0, THEME_COUNT - 1);
  if (web.hasArg("ghours")) {
    int g = web.arg("ghours").toInt();
    if (g == 3 || g == 6 || g == 12 || g == 24) cfg.graph_hours = g;
  }
  cfg.alarms_on        = web.hasArg("alarms");
  cfg.alarm_fast       = web.hasArg("fast");
  cfg.alarm_nodata     = web.hasArg("nodata");
  cfg.quiet_high_night = web.hasArg("qhigh");
  config_save();
  render();                              // the screen follows immediately
  web.sendHeader("Location", "/instellingen?ok=1");
  web.send(303, "text/plain", "");
}

// ---- render dispatch ----------------------------------------------------------------------
static void render() {
  if (cfg.night_mode == 2 && is_night() && millis() >= wake_until_ms &&
      !out_of_range_now())
    draw_night();
  else
    draw_main();
}

// ---- setup / loop ----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== glucose-display v" FW_VERSION " ===");
  config_load();
  if (!display_begin()) Serial.println("display init failed");
  touch_begin();
  beeps_begin();
  battery_begin();
  battery_poll();

  screen_disclaimer();

  WiFi.mode(WIFI_STA);
  if (cfg.wifi_ssid[0] == '\0' || !wifi_connect_ui(cfg.wifi_ssid, cfg.wifi_pass))
    screen_wifi_setup();
  net_ok = (WiFi.status() == WL_CONNECTED);
  Serial.printf("wifi: %s (%s)\n", cfg.wifi_ssid, WiFi.localIP().toString().c_str());

  // Amsterdam time; the reliable source is Dexcom's Date header (dexcom.h),
  // NTP is just a bonus when the network allows it.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  if (cfg.dex_user[0] == '\0') screen_dex_login();
  dex_ok = true;

  if (cfg.web_pass[0] == '\0') web_pass_generate();
  static const char *want[] = { "Origin" };
  web.collectHeaders(want, 1);
  web.on("/", web_root);
  web.on("/api", web_api);
  web.on("/instellingen", HTTP_GET, web_settings_form);
  web.on("/instellingen", HTTP_POST, web_settings_save);
  web.begin();

  last_fetch_ms = 0;                    // fetch immediately
  render();
}

void loop() {
  net_ok = (WiFi.status() == WL_CONNECTED);
  if (!net_ok) {
    WiFi.reconnect();
    delay(2000);
  }
  web.handleClient();
  alarm_pump();

  // Backlight: dim at night (mode 1 and 2), never while out of range, and a
  // tap buys a minute of full brightness.
  {
    bool night = cfg.night_mode > 0 && is_night();
    bool full = !night || out_of_range_now() || millis() < wake_until_ms;
    display_backlight(full ? 100 : cfg.dim_pct);
  }

  if (net_ok && (last_fetch_ms == 0 || millis() - last_fetch_ms > 60000)) {
    int n = dex_fetch(hist, HIST_MAX, 24 * 60);
    if (n > 0) { hist_n = n; dex_ok = true; }
    else if (n < 0) { dex_ok = false; Serial.printf("dexcom: %s\n", dex_last_error); }
    last_fetch_ms = millis();
    render();
    handle_alarms();
  }
  if (millis() - last_draw_ms > 30000) {
    battery_poll();
    render();
    last_draw_ms = millis();
  }

  // morning greeting, once per day between 7 and 10
  {
    time_t nw = time(nullptr);
    struct tm tmnow;
    localtime_r(&nw, &tmnow);
    if (tmnow.tm_year > 100 && tmnow.tm_hour >= 7 && tmnow.tm_hour < 10 &&
        greeted_yday != tmnow.tm_yday && hist_n > 0) {
      greeted_yday = tmnow.tm_yday;
      screen_greeting();
      render();
    }
  }

  if (touch_tapped()) {
    if (in(266, 286, 50, 44)) {          // "..." on the graph card
      beep_click(cfg.volume);
      screen_settings();
      touch_irq = false;
      last_fetch_ms = 0;                 // units/server may have changed
    } else if (in(8, 288, 258, 184)) {   // the graph itself -> statistics
      beep_click(cfg.volume);
      screen_stats();
      touch_irq = false;
    } else {
      // wake the screen; snooze alarms only when one is active
      wake_until_ms = millis() + 60000;
      if (out_of_range_now())
        snooze_until_ms = millis() + 30UL * 60UL * 1000UL;
      beep_click(cfg.volume);
    }
    render();
  }
  delay(20);
}
