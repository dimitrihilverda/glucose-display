// Battery telemetry via the IP5306 power-management chip, if this board
// carries the I2C variant (address 0x75 on the same bus as the touch
// controller). Level comes in 25% steps (the chip reports its LED states),
// charging and charge-complete are status bits. Boards with the non-I2C
// IP5306 simply never ACK the probe and everything here stays silent.
//
// Register map as used by M5Stack's IP5306 support:
//   0x70 bit3 = charger attached / charging
//   0x71 bit3 = charge complete
//   0x78 bits 7..4 = level LEDs, active low
#ifndef BATTERY_H
#define BATTERY_H

#include <Wire.h>

#define IP5306_ADDR 0x75

static bool bat_present = false;
static int bat_pct = -1;
static bool bat_charging = false, bat_full = false;

static int bat_reg(uint8_t reg) {
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)IP5306_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

static void battery_begin() {
  bat_present = (bat_reg(0x78) >= 0);   // Wire is already up (touch bus)
  Serial.printf("battery: IP5306 %s\n", bat_present ? "I2C aanwezig" : "niet uitleesbaar");
}

// Refresh the cached state; call every so often, not per frame.
static void battery_poll() {
  if (!bat_present) return;
  int r70 = bat_reg(0x70), r71 = bat_reg(0x71), r78 = bat_reg(0x78);
  if (r70 < 0 || r71 < 0 || r78 < 0) return;
  bat_full = (r71 & 0x08) != 0;
  bat_charging = (r70 & 0x08) != 0 && !bat_full;
  switch (r78 & 0xF0) {                 // LED bits, active low
    case 0x00: bat_pct = 100; break;
    case 0x80: bat_pct = 75; break;
    case 0xC0: bat_pct = 50; break;
    case 0xE0: bat_pct = 25; break;
    default:   bat_pct = 0;  break;
  }
}

// Small battery glyph with fill level, a bolt while charging. Draws on
// whatever background; colours passed in so it works on light and dark.
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
