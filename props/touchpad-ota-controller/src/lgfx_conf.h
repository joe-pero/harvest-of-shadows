#pragma once
// =============================================================================
// LovyanGFX board config — Hosyond 4.0" ESP32-32E, ST7796S, XPT2046 resistive
// Pins from lcdwiki.com/4.0inch_ESP32-32E_Display (see src/pins.h).
// =============================================================================
#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796  _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
    lgfx::Touch_XPT2046 _touch;

public:
    LGFX() {
        {   // ── SPI bus (LCD): SCLK 14 / MOSI 13 / MISO 12 ──────────────────
            auto c = _bus.config();
            c.spi_host    = SPI2_HOST;      // HSPI
            c.spi_mode    = 0;
            c.freq_write  = 40000000;
            c.freq_read   = 16000000;
            c.spi_3wire   = false;
            c.use_lock    = true;
            c.dma_channel = SPI_DMA_CH_AUTO;
            c.pin_sclk    = 14;
            c.pin_mosi    = 13;
            c.pin_miso    = 12;
            c.pin_dc      = 2;
            _bus.config(c);
            _panel.setBus(&_bus);
        }
        {   // ── ST7796S panel ──────────────────────────────────────────────
            auto c = _panel.config();
            c.pin_cs          = 15;
            c.pin_rst         = -1;        // tied to EN
            c.pin_busy        = -1;
            c.panel_width     = 320;
            c.panel_height    = 480;
            c.offset_x        = 0;
            c.offset_y        = 0;
            c.offset_rotation = 0;
            c.readable        = false;
            c.invert          = true;      // ST7796S on these boards is usually inverted
            c.rgb_order       = false;
            c.dlen_16bit      = false;
            c.bus_shared      = true;
            _panel.config(c);
        }
        {   // ── backlight (PWM on GPIO27) ──────────────────────────────────
            auto c = _light.config();
            c.pin_bl      = 27;
            c.invert      = false;
            c.freq        = 12000;
            c.pwm_channel = 7;
            _light.config(c);
            _panel.setLight(&_light);
        }
        {   // ── XPT2046 resistive touch (shares the LCD SPI bus) ───────────
            auto c = _touch.config();
            // Touch calibration source of truth (2026-09-20) — a plain linear
            // scale, no affine/setTouchCalibrate(). Values below are from a
            // live bench session reading real raw min/max off all 4 corners
            // with Joe pressing the actual board (see PROJECT_LOG.md):
            // TL~(473,289) TR~(3564,365) BL~(430-736,3765-3787) BR~(3522,3741)
            // — x increases L->R, y increases T->B, no axis swap. Small
            // margin added outside the observed range. Do not reintroduce
            // lcd.setTouchCalibrate() (2026-09-19 attempt had a real
            // coordinate-frame bug, see hud.cpp).
            c.x_min      = 400;  c.x_max = 3600;
            c.y_min      = 250;  c.y_max = 3820;
            // DIAGNOSTIC (2026-09-19): pin_int disabled — bench-confirmed the real
            // problem was needing a firm press on this resistive panel, not the
            // IRQ line, but there's no reason to re-enable the gate (see the
            // library's own Touch_XPT2046.cpp:57-60 — it skips the SPI read
            // entirely whenever pin_int reads HIGH) when polling every tick is
            // already cheap. pin_int=-1 forces the real SPI read every time.
            c.pin_int    = -1;
            c.bus_shared = true;
            // offset_rotation=1 bench-confirmed 2026-09-19 (the ARM/DISARM
            // button worked correctly at this value, i.e. no axis swap
            // needed between touch sensor and display at setRotation(3)).
            c.offset_rotation = 1;
            c.spi_host   = SPI2_HOST;
            c.freq       = 1000000;
            c.pin_sclk   = 14;
            c.pin_mosi   = 13;
            c.pin_miso   = 12;
            c.pin_cs     = 33;
            _touch.config(c);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};
