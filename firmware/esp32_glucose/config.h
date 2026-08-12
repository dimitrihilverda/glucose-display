// Persistent settings for the glucose kiosk, all in one NVS namespace.
// Credentials live on-device only (NVS in flash); nothing leaves the board
// except the TLS session to Dexcom.
#ifndef CONFIG_H
#define CONFIG_H

#include <Preferences.h>

struct KioskConfig {
  char wifi_ssid[33];
  char wifi_pass[65];
  char dex_user[65];
  char dex_pass[65];
  bool region_ous;      // true = outside-US server (EU/NL), false = US
  bool use_mmol;        // true = mmol/L (NL default), false = mg/dL
  float low_mmol;       // alarm thresholds, stored in mmol/L
  float high_mmol;
  bool alarms_on;
  uint8_t volume;       // percent, shares the aimelo/vol convention
  bool night_dim;       // dim the backlight between 22:00 and 07:00
  bool accepted;        // disclaimer accepted
};

static KioskConfig cfg;
static Preferences cfg_prefs;

static void config_load() {
  cfg_prefs.begin("glucose", false);
  cfg_prefs.getString("ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
  cfg_prefs.getString("wpass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
  cfg_prefs.getString("duser", cfg.dex_user, sizeof(cfg.dex_user));
  cfg_prefs.getString("dpass", cfg.dex_pass, sizeof(cfg.dex_pass));
  cfg.region_ous = cfg_prefs.getBool("ous", true);
  cfg.use_mmol   = cfg_prefs.getBool("mmol", true);
  cfg.low_mmol   = cfg_prefs.getFloat("low", 4.0f);
  cfg.high_mmol  = cfg_prefs.getFloat("high", 10.0f);
  cfg.alarms_on  = cfg_prefs.getBool("alarm", true);
  cfg.volume     = cfg_prefs.getUChar("vol", 70);
  cfg.night_dim  = cfg_prefs.getBool("ndim", true);
  cfg.accepted   = cfg_prefs.getBool("ok", false);
}

static void config_save() {
  cfg_prefs.putString("ssid", cfg.wifi_ssid);
  cfg_prefs.putString("wpass", cfg.wifi_pass);
  cfg_prefs.putString("duser", cfg.dex_user);
  cfg_prefs.putString("dpass", cfg.dex_pass);
  cfg_prefs.putBool("ous", cfg.region_ous);
  cfg_prefs.putBool("mmol", cfg.use_mmol);
  cfg_prefs.putFloat("low", cfg.low_mmol);
  cfg_prefs.putFloat("high", cfg.high_mmol);
  cfg_prefs.putBool("alarm", cfg.alarms_on);
  cfg_prefs.putUChar("vol", cfg.volume);
  cfg_prefs.putBool("ndim", cfg.night_dim);
  cfg_prefs.putBool("ok", cfg.accepted);
}

#endif
