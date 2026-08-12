// Capacitive touch on the Guition JC3248W535C. The AXS15231B panel controller
// also handles touch, on its own fixed I2C bus -- nothing external to wire.
//
// The keyboard needs positions, not just taps: raw coordinates arrive in the
// panel's native 320x480 portrait frame and are mapped here into the rotated
// landscape frame the canvas draws in (touch_sx / touch_sy), so hit-testing
// uses the same coordinates as drawing. The mapping must match TFT_ROTATION in
// display.h; if taps land mirrored on real hardware, flip TOUCH_FLIP_X/Y.
//
// Protocol: an 8-byte command asks the controller for the first touch point;
// the 8-byte reply carries gesture, point count and the point's coordinates.
// INT falls on a touch event; the ISR only sets a flag and the I2C transaction
// runs in task context, so nothing blocking happens inside the interrupt.
#ifndef TOUCH_H
#define TOUCH_H

#include <Wire.h>

#define TOUCH_SDA  4
#define TOUCH_SCL  8
#define TOUCH_INT  3
#define TOUCH_ADDR 0x3B
// Short enough for typing, long enough to swallow contact bounce and the
// repeat reports of a held finger.
#define TOUCH_DEBOUNCE_MS 180
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0
// Set to 1 to print every accepted tap's raw and mapped coordinates, for
// verifying the mapping against what the keyboard draws.
#define TOUCH_DEBUG 0

static volatile bool touch_irq = false;
static void IRAM_ATTR touch_isr() { touch_irq = true; }

static uint16_t touch_x = 0, touch_y = 0;   // raw, panel-native portrait
static int16_t touch_sx = 0, touch_sy = 0;  // mapped, landscape screen frame

static void touch_begin() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touch_isr, FALLING);
}

// One event per physical tap. The INT flag arms it; the I2C read confirms a
// finger is actually down (the controller also raises INT on release).
static bool touch_tapped() {
  if (!touch_irq) return false;
  touch_irq = false;
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};
  uint8_t buf[8];
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)sizeof(buf)) != sizeof(buf))
    return false;
  for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = Wire.read();
  uint8_t points = buf[1];               // 0 on release events, 0xFF on garbage
  if (points == 0 || points == 0xFF) return false;
  touch_x = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
  touch_y = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];
  // Portrait (rotation 0): the touch frame IS the drawing frame.
  touch_sx = (int16_t)touch_x;
  touch_sy = (int16_t)touch_y;
#if TOUCH_FLIP_X
  touch_sx = PANEL_W - 1 - touch_sx;
#endif
#if TOUCH_FLIP_Y
  touch_sy = PANEL_H - 1 - touch_sy;
#endif
  static uint32_t last_tap = 0;
  uint32_t now = millis();
  if (now - last_tap < TOUCH_DEBOUNCE_MS) return false;
  last_tap = now;
#if TOUCH_DEBUG
  Serial.printf("[touch raw=(%u,%u) screen=(%d,%d)]\n",
                touch_x, touch_y, touch_sx, touch_sy);
#endif
  return true;
}

#endif
