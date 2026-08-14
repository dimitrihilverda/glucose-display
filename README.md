# glucose-display

Bouw je eigen Dexcom-nachtkastje: live glucosewaarden op een ESP32-S3
touchscreen van ~€30, in de stijl van de Dexcom-app. Grote waarde met
trendpijl, grafiek met doelband, dagstatistieken, alarmen via de speaker,
amber nachtklok, webinterface — en de complete setup gebeurt op het scherm
zelf.

**➡️ Flashen zonder toolchain: [de web-flasher](https://dimitrihilverda.github.io/glucose-display/)**
(Chrome/Edge + USB-kabel, klaar in een halve minuut.)

> **Geen medisch hulpmiddel.** Waarden lopen via Dexcom Share en kunnen
> achterlopen, wegvallen of afwijken. Behandelbeslissingen horen op de
> officiële Dexcom-app of een bloedglucosemeter — dit kastje is een
> extraatje, nooit je vangnet.

## Wat je nodig hebt

- **Guition JC3248W535C** — 3.5" ESP32-S3 touchscreen (480×320, capacitief),
  ~€30 op AliExpress
- Een **Dexcom CGM** met Share aan en minstens één volger in de app
- Optioneel: speakertje 8Ω/2W aan de JST 1.25 (alarmen), LiPo 103450 (snoerloos;
  percentage wordt via de ADC uitgelezen)

## Features

- Volledig tweetalig: Nederlands en Engels — taalkeuze bij de eerste start,
  later omschakelbaar in het systeemmenu of via de webinterface
- Waarde-cirkel in app-stijl (geel bij hoog, rood bij laag), trendpijl, delta
- Grafiek 3/6/12/24 uur met doelband en drempellijnen
- Dagoverzicht: tijd-in-bereik, gemiddelde, min/max, aantal lows (24h)
- Alarmen: laag / hoog / snel-dalend / geen-data — herhalend tot je tikt,
  aantal en stijl (zacht/klassiek/fel) instelbaar, hoog-alarm optioneel stil
  's nachts, testmenu om alles vooraf te horen
- Nachtvenster instelbaar: dimmen of amber nachtklok; goedemorgen-groet
- Naam op het display (schoonschrift + hartje of blokletters) en zes
  kleurthema's
- Webinterface op het board-IP: status + alle instellingen (behalve logins),
  achter een apparaat-gegenereerd wachtwoord
- Accubeheer: percentage/laadstatus, automatisch dimmen op accu

## Privacy & security

Dexcom- en WiFi-inloggegevens bestaan alleen in NVS op het apparaat en zijn
via het netwerk niet leesbaar of wijzigbaar; het apparaat praat uitsluitend
via TLS met Dexcom (Share-API, dezelfde route als xDrip+/pydexcom — vereist
Share met ≥1 volger). De webinterface zit achter HTTP Basic Auth (wachtwoord
staat alleen op het credits-scherm), met bescherming tegen DNS-rebinding en
cross-site posts. Bedoeld voor je eigen LAN; niet naar internet openzetten.

De officiële Dexcom developer-API is bewust 1-3 uur vertraagd en daarom
ongeschikt voor live weergave.

## Zelf bouwen

```bash
arduino-cli lib install "GFX Library for Arduino" ArduinoJson
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' --build-path build firmware/esp32_glucose
esptool --chip esp32s3 --port <POORT> --baud 921600 --after watchdog-reset write-flash 0x0 build/esp32_glucose.ino.bootloader.bin 0x8000 build/esp32_glucose.ino.partitions.bin 0xe000 build/boot_app0.bin 0x10000 build/esp32_glucose.ino.bin
```

Let op: het board hangt aan de native USB-Serial/JTAG — gebruik esptool's
`--after watchdog-reset` (de klassieke RTS-reset doet niets). Serial meelezen:
`python tools/capture_serial.py <esptool> log.txt 30 <POORT>`.

## Board-pinnen (JC3248W535C)

| | |
|---|---|
| Display | AXS15231B QSPI: CS=45 SCK=47 D0=21 D1=48 D2=40 D3=39, backlight=1 (PWM) |
| Touch | zelfde chip, I2C: SDA=4 SCL=8 INT=3, adres 0x3B |
| Speaker | NS4168 I2S: BCLK=42 LRCLK=2 DOUT=41 (JST 1.25; de andere JST 1.25 is de accu!) |
| Accu | spanning op GPIO5 via deler ×1,72 (ADC); IP5306-laadchip zonder I2C |

Paneel-eigenaardigheden: geen hardware-rotatie, partial writes onbetrouwbaar →
full-frame canvas in PSRAM; QSPI op 40 MHz (sneller geeft kleur-shimmer).

## Tools

- `tools/ttf2gfx.py` — TTF → GFXfont-headers (Segoe UI/Script/Impact in `fonts.h`)
- `tools/capture_serial.py` — watchdog-reset + serial-log

---
Met liefde gebouwd door Dimitri & Claude. Dank aan de DIY-diabetes-community
(pydexcom, xDrip+) voor de Share-route en aan het JC3248W535-forumvolk voor de
accu-pinout. Dexcom is een merk van Dexcom, Inc.; dit project is er niet aan
verbonden. MIT.
