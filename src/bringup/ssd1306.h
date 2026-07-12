//
// Minimal SSD1306 I2C driver — bring-up test only.
// Blocking I2C (fine for a smoke test; the real status screen will use DMA).
//
#pragma once
#include <cstdint>
#include "hardware/i2c.h"

class SSD1306 {
public:
    static constexpr int WIDTH = 128;
    int height;

    SSD1306(i2c_inst_t *i2c, uint8_t addr, int height = 64)
        : height(height), i2c_(i2c), addr_(addr) {}

    // Returns false if the panel does not ACK its address (bad wiring / wrong addr).
    bool init();

    void clear(bool on = false);
    void set_pixel(int x, int y, bool on = true);
    void fill_rect(int x, int y, int w, int h, bool on = true);
    void rect(int x, int y, int w, int h, bool on = true);
    void draw_char(int x, int y, char c, int scale = 1, bool on = true);
    void draw_text(int x, int y, const char *s, int scale = 1, bool on = true);
    void invert(bool inv);
    void show();

private:
    i2c_inst_t *i2c_;
    uint8_t addr_;
    uint8_t buf_[WIDTH * 64 / 8]; // 1024 bytes max (128x64)
    void cmd(uint8_t c);
};
