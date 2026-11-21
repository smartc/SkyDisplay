#pragma once
// LovyanGFX setup for the Seeed SenseCAP Indicator D1:
// 4" 480x480 ST7701S on a 16-bit RGB bus, FT6336 capacitive touch, PCA9535 I/O expander.
// Pins from Seeed's board support package (SenseCAP_Indicator_ESP32, components/bsp).
//
// The panel needs setup commands over a 3-wire SPI link (GPIO 41 clock, GPIO 48 data) whose chip
// select and reset are on the I/O expander, so begin() drives those over I2C around lcd.init().
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

class LGFX_Indicator : public lgfx::LGFX_Device {
  lgfx::Bus_RGB      _bus;
  lgfx::Panel_ST7701 _panel;   // LovyanGFX's ST7701 table is the one Seeed uses for this panel
  lgfx::Light_PWM    _light;
  lgfx::Touch_FT5x06 _touch;

  static constexpr int I2C_PORT = 0, PIN_SDA = 39, PIN_SCL = 40;
  // Expander outputs (port 0 = pins 0-7, port 1 = pins 8-15)
  static constexpr uint8_t EXP_LCD_CS = 4, EXP_LCD_RST = 5, EXP_TP_RST = 7;
  uint8_t _exp = 0x20;

  void expWrite(uint8_t reg, uint8_t v) { lgfx::i2c::writeRegister8(I2C_PORT, _exp, reg, v, 0, 400000); }
  bool present(uint8_t addr) { return lgfx::i2c::readRegister8(I2C_PORT, addr, 0, 400000).has_value(); }

public:
  uint8_t touchAddr = 0;  // found at begin(); 0 = no touch controller answered

  LGFX_Indicator() {
    {
      auto cfg = _panel.config();
      cfg.memory_width = cfg.panel_width = 480;
      cfg.memory_height = cfg.panel_height = 480;
      cfg.offset_x = cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _panel.config_detail();
      cfg.pin_cs = -1;    // on the expander, handled in begin()
      cfg.pin_sclk = 41;
      cfg.pin_mosi = 48;
      _panel.config_detail(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      // B0-B4, G0-G5, R0-R4
      cfg.pin_d0 = 15; cfg.pin_d1 = 14; cfg.pin_d2 = 13; cfg.pin_d3 = 12; cfg.pin_d4 = 11;
      cfg.pin_d5 = 10; cfg.pin_d6 = 9;  cfg.pin_d7 = 8;  cfg.pin_d8 = 7;  cfg.pin_d9 = 6; cfg.pin_d10 = 5;
      cfg.pin_d11 = 4; cfg.pin_d12 = 3; cfg.pin_d13 = 2; cfg.pin_d14 = 1; cfg.pin_d15 = 0;
      cfg.pin_henable = 18;
      cfg.pin_vsync = 17;
      cfg.pin_hsync = 16;
      cfg.pin_pclk = 21;
      cfg.freq_write = 12000000;  // Seeed's Arduino examples use 12 MHz; leaves PSRAM bandwidth for drawing
      cfg.hsync_polarity = 1; cfg.hsync_pulse_width = 8; cfg.hsync_back_porch = 50; cfg.hsync_front_porch = 10;
      cfg.vsync_polarity = 1; cfg.vsync_pulse_width = 8; cfg.vsync_back_porch = 20; cfg.vsync_front_porch = 10;
      cfg.pclk_active_neg = 0;
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 45;   // active high
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = I2C_PORT;
      cfg.i2c_addr = 0x48;
      cfg.pin_sda = PIN_SDA;
      cfg.pin_scl = PIN_SCL;
      cfg.pin_int = -1;    // INT is on the expander; poll instead (same as the WT32 build)
      cfg.pin_rst = -1;    // reset is on the expander
      cfg.freq = 400000;
      cfg.bus_shared = false;
      cfg.x_min = 0; cfg.x_max = 479;
      cfg.y_min = 0; cfg.y_max = 479;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }

  // Reset the panel and touch through the expander, then initialise the display.
  bool begin() {
    lgfx::i2c::init(I2C_PORT, PIN_SDA, PIN_SCL);
    if (!present(_exp)) _exp = 0x39;  // Seeed's driver falls back to this address
    if (!present(_exp)) { _exp = 0; Serial.println("[lcd] I/O expander not found"); }
    if (_exp) {
      expWrite(0x02, 0xFF); expWrite(0x03, 0xFF);   // outputs high: CS idle, out of reset, RP2040 running
      expWrite(0x06, (uint8_t)~((1 << EXP_LCD_CS) | (1 << EXP_LCD_RST) | (1 << EXP_TP_RST)));  // port 0 directions
      expWrite(0x07, (uint8_t)~((1 << 0) | (1 << 2)));  // port 1: RP2040 reset (8), sensor power (10) as outputs
      expWrite(0x02, (uint8_t)~((1 << EXP_LCD_RST) | (1 << EXP_TP_RST)));  // reset pulse for panel and touch
      delay(10);
      expWrite(0x02, 0xFF);
      delay(200);  // touch controller needs a while after reset before it answers
    }
    // Find the touch controller (0x48 on this board; 0x38 is the usual FT6336 address).
    for (uint8_t a : {0x48, 0x38}) if (present(a)) { touchAddr = a; break; }
    if (touchAddr) { auto cfg = _touch.config(); cfg.i2c_addr = touchAddr; _touch.config(cfg); }
    Serial.printf("[lcd] expander 0x%02x, touch 0x%02x\n", _exp, touchAddr);
    if (_exp) expWrite(0x02, (uint8_t)~(1 << EXP_LCD_CS));   // select the panel for its setup commands
    bool ok = init();
    if (_exp) expWrite(0x02, 0xFF);
    return ok;
  }
};
