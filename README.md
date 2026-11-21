# SkyDisplay

A bedside astronomy forecast display for an ESP32-S3 touchscreen (WT32-SC01 Plus, 3.5" 480×320).
It answers one question at a glance: is tonight, or tomorrow night, worth setting up the imaging rigs?

![Forecast screen](docs/forecast.png)

## What it shows

- **Forecast:** Now / Tonight / Tomorrow tiles (GO, MAYBE, NO) scored for deep-sky imaging, and a 52-hour strip of
  cloud (RDPS, ECMWF, HRRR), transparency, seeing and darkness. A rotating footer shows the moon phase, moonrise and
  moonset, sunset, astronomical dark, sunrise, peak humidity, calmest wind and seeing.
- **Weather:** current conditions, a 24-hour temperature and dew point trace, dew-risk warning, wind, pressure trend
  and a five-day night forecast from a Weather Underground personal weather station.
- **Status:** Wi-Fi, IP address, chart and clock status, and a QR code for the settings page.

Swipe between screens, tap to rotate the footer, long-press to dim.

## Data sources

- **Clear Sky Chart** (Dark Dragons Astronomy): the device downloads the chart GIF and decodes the forecast by
  sampling each cell's colour. There is no text feed for these charts, so a layout change on the site would need an
  update here; the decoder checks the grid and reports a problem rather than showing a wrong forecast.
  Please be considerate with polling (default every 30 minutes) and ask the site's maintainers before using it.
- **Weather Underground PWS API**: needs your own station ID and API key, entered on the settings page.
- **Sun and moon** times are computed on the device.

## Hardware

- WT32-SC01 Plus: ESP32-S3 (2 MB PSRAM, 16 MB flash), ST7796 on an 8-bit parallel bus, FT6336U touch.
- The repository also has HTML mockups for two other Seeed boards in `mockup/`.

## Building

Arduino core esp32 3.3.x with the libraries LovyanGFX, WiFiManager and ArduinoJson.
AnimatedGIF is bundled in `firmware/SkyDisplay/src/gif` with a wider line limit (see `VENDORED.txt`).

```
arduino-cli compile -b esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled firmware/SkyDisplay
arduino-cli upload -p <port> -b <same as above> firmware/SkyDisplay
```

`tools/flash_and_shot.ps1` builds, flashes and saves a screenshot over USB serial.
After the first flash, updates can go over Wi-Fi from the settings page (`/update`).

## First run

1. With no Wi-Fi saved, the display opens a `SkyDisplay-Setup` network. Join it and pick your network.
2. Open the settings page (`http://skydisplay.local/`, or the IP shown on the status screen) and set the
   Clear Sky Chart URL, time zone, NTP servers and, optionally, your Weather Underground station and key.

`NOTES.md` has the design notes: chart decoding, scoring rules, and the problems found along the way.

## Third-party

- Barlow Semi Condensed font: SIL Open Font License 1.1 (`tools/fonts/OFL.txt`); rendered into `fonts.h` by
  `tools/make_fonts.py`.
- AnimatedGIF by Larry Bank: Apache License 2.0 (`firmware/SkyDisplay/src/gif/LICENSE`).
