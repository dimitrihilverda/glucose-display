# Chantie's glucose-display

Een nachtkastje-display voor Dexcom CGM-waarden op een Guition JC3248W535C
(ESP32-S3, 3.5" 480×320 touchscreen, ~€30). Toont de actuele waarde groot in
de stijl van de Dexcom-app: witte cirkel (geel bij hoog, rood bij laag),
trendpijl, 3-uursgrafiek met doelband, en alarmpiepjes via de ingebouwde
speaker. 's Nachts dimt het scherm vanzelf.

> **Geen medisch hulpmiddel.** Waarden lopen via Dexcom Share en kunnen
> achterlopen of wegvallen. Behandelbeslissingen horen op de officiële
> Dexcom-app/meter, niet op dit kastje.

## Hoe het werkt

- **Dexcom Share API** (dezelfde route als xDrip+/pydexcom): realtime, één
  meting per 5 minuten. Vereist Share aan in de Dexcom-app met minstens één
  volger. De officiële developer-API is bewust 1-3 uur vertraagd en daarom
  ongeschikt voor live weergave.
- Eerste keer: disclaimer → WiFi kiezen op het scherm → Dexcom-login typen
  (drie-lagen on-screen keyboard). Alles wordt in NVS op het board bewaard en
  gaat alleen via TLS naar Dexcom.
- De klok komt uit de Date-header van Dexcom's antwoorden (NTP wordt op
  sommige netwerken geblokkeerd).
- Instellingen achter de "⋯" op de grafiekkaart: eenheid (mmol/L of mg/dL),
  alarmgrenzen, volume, alarmen aan/uit, nachtdim (22:00-07:00, 12%; tik =
  1 min vol licht; bij een alarm blijft het scherm altijd vol aan).

## Bouwen & flashen

```bash
arduino-cli lib install "GFX Library for Arduino" ArduinoJson
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' --build-path build firmware/esp32_glucose
esptool --chip esp32s3 --port COM7 --baud 921600 --after watchdog-reset write-flash 0x0 build/esp32_glucose.ino.bootloader.bin 0x8000 build/esp32_glucose.ino.partitions.bin 0xe000 build/boot_app0.bin 0x10000 build/esp32_glucose.ino.bin
```

Let op: het board hangt aan de native USB-Serial/JTAG — de klassieke
RTS-reset doet niets, gebruik esptool's `--after watchdog-reset`. Serial
meelezen: `python tools/capture_serial.py <esptool> log.txt 30 COM7`.

## Board-pinnen (JC3248W535C)

| | |
|---|---|
| Display | AXS15231B QSPI: CS=45 SCK=47 D0=21 D1=48 D2=40 D3=39, backlight=1 (PWM-dimbaar) |
| Touch | zelfde chip, I2C: SDA=4 SCL=8 INT=3, adres 0x3B |
| Speaker | NS4168 I2S: BCLK=42 LRCLK=2 DOUT=41 (JST 1.25; de andere JST 1.25 is de accu) |

Paneel-eigenaardigheden: geen hardware-rotatie en onbetrouwbare partial
writes → alles tekent via een full-frame canvas in PSRAM; QSPI op 40 MHz
(sneller geeft kleur-shimmer).

## Fonts

`tools/ttf2gfx.py` zet een TTF om naar GFXfont-headers (Segoe UI in
`fonts.h`). Ander lettertype? Pas het pad/de grootte in dat script aan en
draai het opnieuw.

---
Gebouwd op de schouders van [slvDev/esp32-ai](https://github.com/slvDev/esp32-ai)'s
board-port-werk. MIT.
