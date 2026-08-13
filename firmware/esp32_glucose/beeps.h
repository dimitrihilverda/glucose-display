// Alarm and UI tones for the glucose kiosk, synthesized square waves straight
// to the NS4168 over I2S -- no SD card involved, the kiosk must be able to
// alarm with nothing but the board. Volume is cfg.volume (percent).
#ifndef BEEPS_H
#define BEEPS_H

#include <ESP_I2S.h>

#define SPK_BCLK  42
#define SPK_LRCLK 2
#define SPK_DOUT  41
#define BEEP_SR   16000

static I2SClass beep_i2s;
static bool beep_present = false;

static bool beeps_begin() {
  beep_i2s.setPins(SPK_BCLK, SPK_LRCLK, SPK_DOUT, -1, -1);
  beep_present = beep_i2s.begin(I2S_MODE_STD, BEEP_SR,
                                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  return beep_present;
}

// One square tone with a short decay so it doesn't click.
static void beep_tone(float freq, int ms, int volume_pct) {
  if (!beep_present) return;
  int n = BEEP_SR * ms / 1000;
  static int16_t buf[512];
  float phase = 0.0f, step = freq / BEEP_SR;
  int done = 0;
  while (done < n) {
    int chunk = min((int)(sizeof(buf) / 2), n - done);
    for (int i = 0; i < chunk; i++) {
      float env = 1.0f - (float)(done + i) / n * 0.6f;
      float s = (phase < 0.5f ? 1.0f : -1.0f) * env;
      buf[i] = (int16_t)(s * 20000.0f * volume_pct / 100);
      phase += step;
      if (phase >= 1.0f) phase -= 1.0f;
    }
    beep_i2s.write((uint8_t *)buf, chunk * 2);
    done += chunk;
  }
}

static void beep_click(int vol)  { beep_tone(1800, 15, vol); }
static void beep_ok(int vol)     { beep_tone(659, 70, vol); beep_tone(880, 90, vol); }
// Alarms follow the medical-device idiom: low = urgent descending triple,
// high = calm double. Distinct by pattern, not just pitch.
static void beep_alarm_low(int vol) {
  for (int i = 0; i < 3; i++) { beep_tone(740, 180, vol); beep_tone(587, 180, vol); delay(80); }
}
static void beep_alarm_high(int vol) {
  beep_tone(880, 160, vol); delay(90); beep_tone(880, 160, vol);
}
// Fast drop: two quick descending pairs -- urgent but distinct from LOW.
static void beep_fastdrop(int vol) {
  beep_tone(988, 110, vol); beep_tone(740, 140, vol);
  delay(120);
  beep_tone(988, 110, vol); beep_tone(740, 140, vol);
}
// No data: two soft low blips, more "hey" than "help".
static void beep_nodata(int vol) {
  beep_tone(440, 120, vol * 2 / 3); delay(150); beep_tone(440, 120, vol * 2 / 3);
}

#endif
