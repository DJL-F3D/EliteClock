#pragma once
// =============================================================================
//  display_config.h — LovyanGFX hardware configuration for Elite Clock
// =============================================================================
// LGFX_USE_V1 is defined via -DLGFX_USE_V1 in platformio.ini build_flags
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {

    lgfx::Panel_ST7796    _panel;   // Try ST7796 — close relative of ILI9488, better rotation
    lgfx::Bus_SPI         _bus;
    lgfx::Light_PWM       _light;
    lgfx::Touch_XPT2046   _touch;

public:
    LGFX(void) {

        // ── SPI bus ──────────────────────────────────────────────────────────
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;   // 40 MHz — safe with LovyanGFX, faster render
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = 0;          // disable DMA — ESP32-C3 DMA can stall on SPI
                                          // polling mode is reliable and fast enough at 40MHz
            cfg.pin_sclk    =  2;
            cfg.pin_mosi    =  4;
            cfg.pin_miso    =  6;    // needed for touch readback
            cfg.pin_dc      =  1;    // DC / RS
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        // ── ILI9488 panel ────────────────────────────────────────────────────
        {
            auto cfg           = _panel.config();
            cfg.pin_cs         =  5;
            cfg.pin_rst        =  0;
            cfg.pin_busy       = -1;
            cfg.memory_width   = 320;
            cfg.memory_height  = 480;
            cfg.panel_width    = 320;
            cfg.panel_height   = 480;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            // offset_rotation bakes the rotation into the init() sequence itself.
            // Unlike setRotation() (which sends MADCTL after init), this value
            // is applied while the display is first being configured — before
            // it turns on. If setRotation() has no effect, this usually works.
            // 1 = landscape (90° CW from portrait-native)
            cfg.offset_rotation = 1;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable       = true;
            cfg.invert         = false;
            cfg.rgb_order      = false;
            cfg.dlen_16bit     = false;
            cfg.bus_shared     = true;
            _panel.config(cfg);
        }

        // ── Backlight (PWM) ───────────────────────────────────────────────────
        {
            auto cfg        = _light.config();
            cfg.pin_bl      = 8;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }

        // ── XPT2046 touch ─────────────────────────────────────────────────────
        // Calibration values (300/3800) are typical raw ADC ranges.
        // After first boot, call tft.calibrateTouch() to get precise values
        // and store them. For now these defaults give usable accuracy.
        {
            auto cfg            = _touch.config();
            cfg.x_min           =  300;
            cfg.x_max           = 3800;
            cfg.y_min           =  300;
            cfg.y_max           = 3800;
            cfg.pin_int         =   10;   // T_IRQ
            cfg.bus_shared      = true;   // shared SPI with display
            cfg.offset_rotation =    0;
            cfg.spi_host        = SPI2_HOST;
            cfg.freq            = 2500000;
            cfg.pin_sclk        =  2;
            cfg.pin_mosi        =  4;
            cfg.pin_miso        =  6;
            cfg.pin_cs          =  9;     // T_CS
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }

        setPanel(&_panel);
    }
};
