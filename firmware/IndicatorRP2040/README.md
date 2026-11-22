# IndicatorRP2040: buzzer firmware for the SenseCAP Indicator

The Indicator's buzzer is wired to its RP2040 co-processor, not the ESP32-S3 that runs SkyDisplay. Seeed's factory
RP2040 firmware only beeps once at power-on and ignores beep commands, so SkyDisplay's alarm sound needs this small
replacement. It does only one thing: play the tones the ESP32-S3 asks for over their shared UART. It leaves the
RP2040's sensors off (SkyDisplay doesn't use them) and stays silent at power-on.

Only the Indicator needs this. The WT32-SC01 Plus has no RP2040.

## Build

Install the Raspberry Pi Pico core (Earle Philhower) in the Arduino IDE, with this URL under Preferences > Additional
boards manager URLs:

```
https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
```

Then build for **Seeed INDICATOR RP2040** (its flash and USB settings match the board), or with arduino-cli:

```
arduino-cli compile -b rp2040:rp2040:seeed_indicator_rp2040 --output-dir build firmware/IndicatorRP2040
```

The file to flash is `build/IndicatorRP2040.ino.uf2`.

## Flash

1. Unplug the Indicator.
2. With a needle or straightened paper clip, press and hold the button inside the small hole on the back.
3. Keep holding it, plug the Indicator into your computer with USB-C, then let go.
4. A drive called **RPI-RP2** appears. Copy `IndicatorRP2040.ino.uf2` onto it. The drive disappears by itself when
   it's done, and the RP2040 starts the new firmware.

The ESP32-S3 side (SkyDisplay itself) is not touched. Its settings page then shows **Buzzer ready (RP2040 v1)** in the
Notifications section, and the status screen stops warning about the buzzer.

## Put Seeed's firmware back

Same steps, with Seeed's file instead:
https://github.com/Seeed-Solution/SenseCAP_Indicator_RP2040/releases/tag/v1.0.0
(`terminal_rp2040_v1.0.0.uf2`).

## Protocol

Serial1 (RP2040 TX GPIO16, RX GPIO17; ESP32-S3 TX GPIO19, RX GPIO20) at 115200, 8N1. Packets are COBS-encoded and end
in 0x00, as in Seeed's firmware: one type byte, then a little-endian payload.

| Type | Name | Payload | |
|---|---|---|---|
| 0xA1 | BEEP_ON | u32 ms | one beep at 2.7 kHz (Seeed's packet) |
| 0xA2 | BEEP_OFF | | stop any sound |
| 0xA6 | TONE | u16 Hz, u16 ms | one tone; ends by itself |
| 0xA7 | PING | | answered with HELLO |
| 0xC7 | HELLO | u8 version | every 2 s until the first PING, then in answer to PING |

The ESP32-S3 times the alarm pattern and sends one tone at a time, so the buzzer can't get stuck on if the ESP32-S3
stalls.
