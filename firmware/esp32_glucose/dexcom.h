// Dexcom Share client. This is the same undocumented-but-stable API every DIY
// display uses (pydexcom, xDrip+, Sugarmate): username/password in, real-time
// EGVs out, one reading per 5 minutes. Requires Share to be enabled in the
// Dexcom app with at least one follower. The official developer API is not
// usable here: its data is delayed 1-3 hours by design.
//
// Flow: AuthenticatePublisherAccount -> account GUID
//       LoginPublisherAccountById    -> session GUID (kept until it expires)
//       ReadPublisherLatestGlucoseValues?sessionId=...&minutes=&maxCount=
#ifndef DEXCOM_H
#define DEXCOM_H

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define DEX_APP_ID "d89443d2-327c-4a6f-89e5-496bbb0317db"

struct Egv {
  time_t t;        // reading time, unix seconds
  int mgdl;
  int8_t trend;    // index into DEX_TREND_* tables, -1 unknown
};

static const char *DEX_TRENDS[] = {
  "None", "DoubleUp", "SingleUp", "FortyFiveUp", "Flat",
  "FortyFiveDown", "SingleDown", "DoubleDown", "NotComputable", "RateOutOfRange"
};
#define DEX_TREND_COUNT 10

static char dex_session[48] = "";
static char dex_last_error[96] = "";

static const char *dex_host() {
  return cfg.region_ous ? "shareous1.dexcom.com" : "share2.dexcom.com";
}

// Set the system clock from an HTTP Date header ("Wed, 12 Aug 2026 13:29:05
// GMT"). NTP turned out to be blocked on some networks, and a wrong clock
// wrecks every age and graph calculation -- Dexcom's own response carries
// perfectly good wall time, so use that.
static void dex_sync_clock(const String &date) {
  if (date.length() < 27) return;
  char mon[4] = {0};
  int day, yr, hh, mm, ss;
  if (sscanf(date.c_str() + 5, "%d %3s %d %d:%d:%d", &day, mon, &yr, &hh, &mm, &ss) != 6)
    return;
  static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *p = strstr(M, mon);
  if (!p) return;
  int m = (int)(p - M) / 3 + 1;                  // 1..12
  int y = yr - (m <= 2 ? 1 : 0);                 // days_from_civil
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  time_t t = (time_t)(era * 146097L + (long)doe - 719468L) * 86400
           + hh * 3600 + mm * 60 + ss;
  if (labs((long)(t - time(nullptr))) > 120) {
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
  }
}

// POST a JSON body, return HTTP code; response body in `resp`.
static int dex_post(const char *path, const String &body, String &resp) {
  WiFiClientSecure client;
  client.setInsecure();               // v1: no cert pinning; LAN->Dexcom TLS
  HTTPClient http;
  String url = String("https://") + dex_host() + path;
  if (!http.begin(client, url)) { resp = ""; return -1; }
  static const char *want_hdrs[] = { "Date" };
  http.collectHeaders(want_hdrs, 1);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0");
  int code = http.POST(body);
  resp = http.getString();
  if (http.hasHeader("Date")) dex_sync_clock(http.header("Date"));
  http.end();
  return code;
}

// Strip the quotes Dexcom puts around bare GUID responses.
static void dex_unquote(String &s) {
  s.trim();
  if (s.length() >= 2 && s[0] == '"' && s[s.length() - 1] == '"')
    s = s.substring(1, s.length() - 1);
}

static bool dex_login() {
  dex_session[0] = '\0';
  JsonDocument d;
  d["accountName"] = cfg.dex_user;
  d["password"] = cfg.dex_pass;
  d["applicationId"] = DEX_APP_ID;
  String body; serializeJson(d, body);

  String resp;
  int code = dex_post("/ShareWebServices/Services/General/AuthenticatePublisherAccount", body, resp);
  if (code != 200) {
    snprintf(dex_last_error, sizeof(dex_last_error), "auth %d: %.60s", code, resp.c_str());
    return false;
  }
  dex_unquote(resp);
  String accountId = resp;

  JsonDocument d2;
  d2["accountId"] = accountId;
  d2["password"] = cfg.dex_pass;
  d2["applicationId"] = DEX_APP_ID;
  String body2; serializeJson(d2, body2);
  code = dex_post("/ShareWebServices/Services/General/LoginPublisherAccountById", body2, resp);
  if (code != 200) {
    snprintf(dex_last_error, sizeof(dex_last_error), "login %d: %.60s", code, resp.c_str());
    return false;
  }
  dex_unquote(resp);
  strncpy(dex_session, resp.c_str(), sizeof(dex_session) - 1);
  dex_last_error[0] = '\0';
  return dex_session[0] != '\0';
}

static int8_t dex_trend_index(const char *s) {
  for (int i = 0; i < DEX_TREND_COUNT; i++)
    if (strcmp(s, DEX_TRENDS[i]) == 0) return (int8_t)i;
  return -1;
}

// Fetch newest-first EGVs. Returns count, 0 on none, -1 on error (relogs once
// on an expired session).
static int dex_fetch(Egv *out, int maxn, int minutes) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (dex_session[0] == '\0' && !dex_login()) return -1;
    char path[160];
    snprintf(path, sizeof(path),
             "/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues"
             "?sessionId=%s&minutes=%d&maxCount=%d", dex_session, minutes, maxn);
    String resp;
    int code = dex_post(path, "", resp);
    if (code == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        snprintf(dex_last_error, sizeof(dex_last_error), "json parse");
        return -1;
      }
      JsonArray arr = doc.as<JsonArray>();
      int n = 0;
      for (JsonObject o : arr) {
        if (n >= maxn) break;
        const char *wt = o["WT"] | "";          // "Date(1700000000000)"
        const char *lp = strchr(wt, '(');
        out[n].t = lp ? (time_t)(strtoll(lp + 1, nullptr, 10) / 1000) : 0;
        out[n].mgdl = o["Value"] | 0;
        out[n].trend = dex_trend_index(o["Trend"] | "None");
        n++;
      }
      dex_last_error[0] = '\0';
      return n;
    }
    // most likely an expired session: force a fresh login and retry once
    snprintf(dex_last_error, sizeof(dex_last_error), "fetch %d: %.60s", code, resp.c_str());
    dex_session[0] = '\0';
  }
  return -1;
}

#endif
