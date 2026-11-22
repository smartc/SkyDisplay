#pragma once
// Which board this build is for.
//
// Picked automatically from the PSRAM setting the board needs anyway:
//   SenseCAP Indicator D1: 4" 480x480, octal (OPI) PSRAM  -> BOARD_INDICATOR
//   WT32-SC01 Plus:      3.5" 480x320, quad (QSPI) PSRAM  -> BOARD_WT32SC01PLUS
// To force one, uncomment a line below.
// #define BOARD_INDICATOR 1
// #define BOARD_WT32SC01PLUS 1

#include <sdkconfig.h>

#if !defined(BOARD_INDICATOR) && !defined(BOARD_WT32SC01PLUS)
  #if defined(CONFIG_SPIRAM_MODE_OCT) && CONFIG_SPIRAM_MODE_OCT
    #define BOARD_INDICATOR 1
  #else
    #define BOARD_WT32SC01PLUS 1
  #endif
#endif

#if defined(BOARD_INDICATOR)
  #define BOARD_NAME   "SenseCAP Indicator"
  #define SCREEN_W     480
  #define SCREEN_H     480
  #define SERIAL_BAUD  460800   // USB-serial chip (CH340), not native USB; 921600 drops bytes on long transfers
  // The RGB panel is refreshed continuously from a frame buffer in PSRAM. While flash is being written
  // (firmware updates), PSRAM can't be read, the refresh misses its timing and the picture tears until
  // the next restart. So the backlight goes off during updates.
  #define BLANK_DURING_FLASH_WRITES 1
  // Sound: a passive piezo buzzer on the RP2040 co-processor. It needs SkyDisplay's own RP2040 firmware
  // (firmware/IndicatorRP2040); the ESP32-S3 sends it tones over UART (TX GPIO19, RX GPIO20).
  #define BOARD_HAS_SOUND 1
#else
  #define BOARD_NAME   "WT32-SC01 Plus"
  #define SCREEN_W     480
  #define SCREEN_H     320
  #define SERIAL_BAUD  115200   // native USB; the baud rate doesn't matter
  #define BLANK_DURING_FLASH_WRITES 0   // the ST7796 keeps its own copy of the picture
  #define BOARD_HAS_SOUND 0             // speaker connector (I2S amplifier): not supported yet
#endif

// Square screen: room for the full footer and a today panel on the weather screen.
#define SCREEN_TALL (SCREEN_H >= 400)
