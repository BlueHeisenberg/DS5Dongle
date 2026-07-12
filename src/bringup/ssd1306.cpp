//
// Minimal SSD1306 I2C driver — bring-up test only.
//
#include "ssd1306.h"
#include "font5x7.h"
#include <cstring>

void SSD1306::cmd(uint8_t c) {
    uint8_t b[2] = {0x00, c}; // 0x00 = command control byte
    i2c_write_blocking(i2c_, addr_, b, 2, false);
}

bool SSD1306::init() {
    // Probe: does anything ACK at this address?
    uint8_t probe = 0x00;
    if (i2c_write_blocking(i2c_, addr_, &probe, 1, false) < 0) {
        return false;
    }

    const uint8_t seq[] = {
        0xAE,                                        // display off
        0xD5, 0x80,                                  // clock divide
        0xA8, static_cast<uint8_t>(height - 1),      // multiplex ratio
        0xD3, 0x00,                                  // display offset
        0x40,                                        // start line 0
        0x8D, 0x14,                                  // charge pump on
        0x20, 0x00,                                  // horizontal addressing
        0xA1,                                        // segment remap
        0xC8,                                        // COM scan direction
        0xDA, static_cast<uint8_t>(height == 64 ? 0x12 : 0x02), // COM pins
        0x81, 0xCF,                                  // contrast
        0xD9, 0xF1,                                  // pre-charge
        0xDB, 0x40,                                  // VCOMH deselect
        0xA4,                                        // resume to RAM content
        0xA6,                                        // normal (not inverted)
        0xAF,                                        // display on
    };
    for (uint8_t c : seq) cmd(c);

    clear(false);
    show();
    return true;
}

void SSD1306::clear(bool on) {
    memset(buf_, on ? 0xFF : 0x00, WIDTH * (height / 8));
}

void SSD1306::set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= height) return;
    int idx = x + (y / 8) * WIDTH;
    uint8_t bit = 1u << (y % 8);
    if (on) buf_[idx] |= bit;
    else buf_[idx] &= ~bit;
}

void SSD1306::fill_rect(int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            set_pixel(x + i, y + j, on);
}

void SSD1306::rect(int x, int y, int w, int h, bool on) {
    for (int i = 0; i < w; i++) {
        set_pixel(x + i, y, on);
        set_pixel(x + i, y + h - 1, on);
    }
    for (int j = 0; j < h; j++) {
        set_pixel(x, y + j, on);
        set_pixel(x + w - 1, y + j, on);
    }
}

void SSD1306::draw_char(int x, int y, char c, int scale, bool on) {
    if (c >= 'a' && c <= 'z') c -= 32;      // fold to uppercase
    if (c < 0x20 || c > 0x5F) c = 0x20;      // out of range -> space
    if (scale < 1) scale = 1;
    const uint8_t *g = FONT5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1u << row))) continue;
            if (scale == 1) set_pixel(x + col, y + row, on);
            else fill_rect(x + col * scale, y + row * scale, scale, scale, on);
        }
    }
}

void SSD1306::draw_text(int x, int y, const char *s, int scale, bool on) {
    if (scale < 1) scale = 1;
    while (*s) {
        draw_char(x, y, *s++, scale, on);
        x += 6 * scale; // 5px glyph + 1px gap, scaled
    }
}

void SSD1306::invert(bool inv) {
    cmd(inv ? 0xA7 : 0xA6);
}

void SSD1306::show() {
    int pages = height / 8;
    cmd(0x21); cmd(0); cmd(WIDTH - 1);   // column range
    cmd(0x22); cmd(0); cmd(pages - 1);   // page range

    int n = WIDTH * pages;
    uint8_t tmp[1 + WIDTH * 8];          // up to 1025 bytes
    tmp[0] = 0x40;                       // 0x40 = data control byte
    memcpy(tmp + 1, buf_, n);
    i2c_write_blocking(i2c_, addr_, tmp, n + 1, false);
}
