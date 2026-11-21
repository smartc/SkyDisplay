# SkyDisplay

A bedside astronomy forecast display for an ESP32-S3 touchscreen (WT32-SC01 Plus, 3.5" 480×320).
It answers one question at a glance: is tonight, or tomorrow night, worth setting up the imaging rigs?

![Forecast screen](docs/forecast.png)

## What it shows

- **Forecast:** Now / Tonight / Tomorrow tiles (GO, MAYBE, NO) scored for deep-sky imaging, and a 52-hour strip of
  cloud (RDPS, ECMWF, HRRR), transparency, seeing and darkness. A rotating footer shows the moon phase, moonrise and
  moonset, sunset, astronomical dark, sunrise, peak humidity, calmest wind and seeing.
- **Weather:** current conditions, a 24-hour temperature and dew point trace, dew-risk warning, wind, pressure trend,
  tonight's cloud cover, and a strip for tonight plus the next four days (day high and low, night-time cloud and
  chance of precipitation), from a Weather Underground personal weather station.
- **Status:** Wi-Fi, IP address, chart, clock sync and weather status, and a QR code for the settings page.

Swipe between screens, tap to rotate the footer, long-press to dim. When the screen is dimmed, the first tap wakes it.

## Data sources

- **Clear Sky Chart** (Dark Dragons Astronomy): the device downloads the chart GIF and decodes the forecast by
  sampling each cell's colour. There is no text feed for these charts, so a layout change on the site would need an
  update here; the decoder checks the grid and reports a problem rather than showing a wrong forecast.
  Only Dark Dragons chart pages (`https://cds.darkdragonsastro.com/g/<chart id>.html`) are supported; the classic
  cleardarksky.com charts use a different layout.
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

After the first flash over USB, updates can go over Wi-Fi: upload the compiled `.bin` (Arduino IDE: Sketch >
Export Compiled Binary, or `arduino-cli compile --output-dir`) on the settings page under Update firmware (`/update`).

Tools (Python 3 with Pillow):
- `tools/make_fonts.py` regenerates `fonts.h` from the TrueType files in `tools/fonts`.
- `tools/flash_and_shot.ps1` (PowerShell) builds, flashes and saves a screenshot over USB serial;
  `tools/shot_to_png.py` converts the raw screenshot.

## First run

1. With no Wi-Fi saved, the display opens a `SkyDisplay-Setup` network and shows two QR codes: one joins that
   network from a phone camera, the other opens the setup page (`http://192.168.4.1/`). Pick your Wi-Fi network there.
2. Once connected, the display shows a QR code for its settings page (`http://skydisplay.local/`, or its IP).
   Enter your Clear Sky Chart ID (e.g. `abcde` from `https://cds.darkdragonsastro.com/g/abcde.html`; the full link
   works too), your time zone and NTP servers and, optionally, your Weather Underground station and key.
3. Latitude and longitude come from the chart by default. **Use my location** on the settings page fills them in
   from your phone's GPS instead (see Limitations about the certificate warning).

## Limitations

- HTTPS certificates are not verified yet (the chart site and the weather API are reached with TLS, unverified).
- The settings page is plain HTTP on your local network, with an optional password (user `admin`). Keep the display
  on a trusted network.
- Browsers only share GPS location with HTTPS pages, so **Use my location** opens a small page the display serves
  over HTTPS (port 443) with a self-signed certificate it generates at first boot (ECDSA P-256, stored on the device;
  no key is in this repository). Expect a one-time "connection not private" warning to click through.
- The Weather Underground API key is stored in the device's flash. It is never shown on the page or in logs.
- Chart decoding depends on the chart's current pixel layout.

## License

MIT, see `LICENSE`. That covers this project's own code; the third-party parts below keep their own licences
and are not covered by it.

## Third-party

- Barlow Semi Condensed font: SIL Open Font License 1.1 (`tools/fonts/OFL.txt`); rendered into `fonts.h` by
  `tools/make_fonts.py`.
- AnimatedGIF by Larry Bank: Apache License 2.0 (`firmware/SkyDisplay/src/gif/LICENSE`).
