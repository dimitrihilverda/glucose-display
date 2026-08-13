// Persistent settings for the glucose kiosk, all in one NVS namespace.
// Credentials live on-device only (NVS in flash); nothing leaves the board
// except the TLS session to Dexcom.
#ifndef CONFIG_H
#define CONFIG_H

#include <Preferences.h>

struct KioskConfig {
  char display_name[25];   // owner's first name, rendered in script
  char wifi_ssid[33];
  char wifi_pass[65];
  char dex_user[65];
  char dex_pass[65];
  bool region_ous;      // true = outside-US server (EU/NL), false = US
  bool use_mmol;        // true = mmol/L (NL default), false = mg/dL
  float low_mmol;       // alarm thresholds, stored in mmol/L
  float high_mmol;
  bool alarms_on;
  uint8_t volume;       // percent
  uint8_t night_mode;   // at 22:00-07:00: 0 = off, 1 = dim, 2 = amber clock
  uint8_t dim_pct;      // night brightness, percent
  uint8_t graph_hours;  // main-graph window: 3, 6, 12 or 24
  bool alarm_fast;      // alarm on fast drops even inside the range
  bool alarm_nodata;    // alarm when readings stop coming in
  bool quiet_high_night;// suppress the HIGH alarm at night (low always sounds)
  bool accepted;        // disclaimer accepted
};

static KioskConfig cfg;
static Preferences cfg_prefs;

static void config_load() {
  cfg_prefs.begin("glucose", false);
  cfg_prefs.getString("name", cfg.display_name, sizeof(cfg.display_name));
  if (cfg.display_name[0] == '\0') strcpy(cfg.display_name, "Chantie");
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
  cfg.night_mode = cfg_prefs.getUChar("nmode", 1);
  cfg.dim_pct    = cfg_prefs.getUChar("dimpct", 12);
  cfg.graph_hours = cfg_prefs.getUChar("ghours", 3);
  cfg.alarm_fast = cfg_prefs.getBool("afast", true);
  cfg.alarm_nodata = cfg_prefs.getBool("anodata", true);
  cfg.quiet_high_night = cfg_prefs.getBool("qhigh", true);
  cfg.accepted   = cfg_prefs.getBool("ok", false);
}

static void config_save() {
  cfg_prefs.putString("name", cfg.display_name);
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
  cfg_prefs.putUChar("nmode", cfg.night_mode);
  cfg_prefs.putUChar("dimpct", cfg.dim_pct);
  cfg_prefs.putUChar("ghours", cfg.graph_hours);
  cfg_prefs.putBool("afast", cfg.alarm_fast);
  cfg_prefs.putBool("anodata", cfg.alarm_nodata);
  cfg_prefs.putBool("qhigh", cfg.quiet_high_night);
  cfg_prefs.putBool("ok", cfg.accepted);
}

#endif
