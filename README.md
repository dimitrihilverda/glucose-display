# glucose-display

Build your own Dexcom bedside display: live glucose readings on a ~€30
ESP32-S3 touchscreen, in the idiom of the Dexcom app. Large value with trend
arrow, graph with target band, daily statistics, alarms through a speaker,
amber night clock, web interface — and the entire setup happens on the screen
itself.

**➡️ Flash it without a toolchain: [the web flasher](https://dimitrihilverda.github.io/glucose-display/)**
(Chrome/Edge + a USB cable, done in half a minute.)

> **Not a medical device.** Readings travel via Dexcom Share and can be
> delayed, missing or wrong. Treatment decisions belong on the official Dexcom
> app or a blood glucose meter — this display is a nicety, never your safety
> net.

## What you need

- **Guition JC3248W535C** — 3.5" ESP32-S3 touchscreen (480×320, capacitive),
  ~€30 on AliExpress
- A **Dexcom CGM** with Share enabled and at least one follower in the app
- Optional: an 8Ω/2W speaker on the JST 1.25 header (alarms), a LiPo 103450
  for cordless use (percentage is read through the ADC)

## Features

- Fully bilingual: English and Dutch — language picker on first boot,
  switchable later in the system menu or through the web interface
- App-style value circle (yellow when high, red when low), trend arrow, delta
- Graph over 3/6/12/24 hours with target band and threshold lines
- Daily overview: time in range, average, min/max, number of lows (24h)
- Alarms: low / high / fast-drop / no-data — repeating until you tap, with
  configurable count and style (soft/classic/loud), high alarm optionally
  silent at night, and a test menu to hear everything up front
- Configurable night window: dim or amber night clock; good-morning greeting
- A name on the display (script with a heart, or block letters) and six colour
  themes
- Web interface on the board's IP: status plus every setting (except logins),
  behind a device-generated password
- Battery handling: percentage, charge state, automatic dimming on battery

## Privacy & security

Dexcom and WiFi credentials exist only in NVS on the device and can never be
read or changed over the network; the device talks exclusively to Dexcom over
TLS (the Share API — the same route as xDrip+/pydexcom, which requires Share
with ≥1 follower). The web interface sits behind HTTP basic auth (the password
appears only on the credits screen), with protection against DNS rebinding and
cross-site posts. Meant for your own LAN; do not expose the device to the
internet.

The official Dexcom developer API is delayed by 1-3 hours by design and is
therefore unsuitable for a live display.

## Building it yourself

```bash
arduino-cli lib install "GFX Library for Arduino" ArduinoJson
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' --build-path build firmware/esp32_glucose
esptool --chip esp32s3 --port <PORT> --baud 921600 --after watchdog-reset write-flash 0x0 build/esp32_glucose.ino.bootloader.bin 0x8000 build/esp32_glucose.ino.partitions.bin 0xe000 build/boot_app0.bin 0x10000 build/esp32_glucose.ino.bin
```

Note: the board hangs off native USB-Serial/JTAG — use esptool's
`--after watchdog-reset` (the classic RTS reset does nothing). To follow the
serial output: `python tools/capture_serial.py <esptool> log.txt 30 <PORT>`.

## Board pins (JC3248W535C)

| | |
|---|---|
| Display | AXS15231B QSPI: CS=45 SCK=47 D0=21 D1=48 D2=40 D3=39, backlight=1 (PWM) |
| Touch | same chip, I2C: SDA=4 SCL=8 INT=3, address 0x3B |
| Speaker | NS4168 I2S: BCLK=42 LRCLK=2 DOUT=41 (JST 1.25; the other JST 1.25 is the battery!) |
| Battery | voltage on GPIO5 through a ×1.72 divider (ADC); IP5306 charger without I2C |

Panel quirks: no hardware rotation, partial writes unreliable → full-frame
canvas in PSRAM; QSPI at 40 MHz (faster causes colour shimmer).

## Tools

- `tools/ttf2gfx.py` — TTF → GFXfont headers (Segoe UI/Script/Impact in `fonts.h`)
- `tools/capture_serial.py` — watchdog reset + serial log

## Adding a language

Every user-visible string goes through the `TR(nl, en)` macro in `config.h`,
with the choice stored in NVS. German and French need a wider table *and*
accented glyphs (ä, é, ç), which means extending the charset in
`tools/ttf2gfx.py` and regenerating `fonts.h` first.

---
Built with love by Dimitri & Claude. Thanks to the DIY diabetes community
(pydexcom, xDrip+) for the Share route, and to the JC3248W535 forum crowd for
the battery pinout. Dexcom is a trademark of Dexcom, Inc.; this project is not
affiliated with it. MIT.
