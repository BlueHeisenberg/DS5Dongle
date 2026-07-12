//
// Hardware bring-up smoke test for the Xbox-passthrough build.
// Drives the onboard LED and the I2C OLED (I2C1 on GP10/GP11).
// This does NOT touch the PIO-USB pins (GP2/GP3) or BT — it only proves
// the display + LED path before we build the real firmware on top.
//
#include <cstdio>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/i2c.h"
#include "ssd1306.h"

// ---- OLED (matches the wiring diagram) ----
#define I2C_PORT   i2c1
#define PIN_SDA    10        // GP10 = I2C1 SDA (pin 14)
#define PIN_SCL    11        // GP11 = I2C1 SCL (pin 15)
#define OLED_ADDR  0x3C      // try 0x3D if the panel doesn't ACK
#define OLED_H     32        // 0.91" 128x32 panel

// ---- External status LEDs (fill in once you tell me the GPIOs) ----
// e.g. static const uint EXT_LEDS[] = {16, 17, 18};
static const uint EXT_LEDS[] = {};
static constexpr int N_EXT = sizeof(EXT_LEDS) / sizeof(EXT_LEDS[0]);

int main() {
    stdio_init_all();
    sleep_ms(400);
    printf("\n[bringup] start — Pico 2 W passthrough hardware test\n");

    // Onboard LED lives on the CYW43 chip on the Pico 2 W.
    bool led_ok = (cyw43_arch_init() == 0);
    printf("[bringup] onboard LED (cyw43): %s\n", led_ok ? "ok" : "INIT FAILED");

    // External LEDs (none until configured).
    for (int i = 0; i < N_EXT; i++) {
        gpio_init(EXT_LEDS[i]);
        gpio_set_dir(EXT_LEDS[i], GPIO_OUT);
    }
    printf("[bringup] external LEDs configured: %d\n", N_EXT);

    // I2C1 for the OLED.
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();
    printf("[bringup] OLED @0x%02X: %s\n", OLED_ADDR,
           oled_ok ? "ok" : "NO ACK (check wiring / try 0x3D)");

    // Panel proof: everything on for ~700 ms, LED on too.
    if (oled_ok) { oled.clear(true); oled.show(); }
    if (led_ok) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    for (int i = 0; i < N_EXT; i++) gpio_put(EXT_LEDS[i], 1);
    sleep_ms(700);

    // Animation: border + bouncing box; blink every LED each frame.
    const int bw = 24, bh = OLED_H / 3;
    int x = 1, y = (OLED_H - bh) / 2, dx = 2;
    bool phase = false;
    uint32_t frame = 0;

    while (true) {
        if (oled_ok) {
            oled.clear(false);
            oled.rect(0, 0, SSD1306::WIDTH, OLED_H, true);      // border
            oled.fill_rect(x, y, bw, bh, true);                 // moving box
            oled.fill_rect(2, 2, (int)(frame % 60), 3, true);   // activity bar
            oled.show();
        }

        x += dx;
        if (x <= 1 || x + bw >= SSD1306::WIDTH - 1) dx = -dx;

        phase = !phase;
        if (led_ok) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, phase);
        for (int i = 0; i < N_EXT; i++) gpio_put(EXT_LEDS[i], phase);

        if ((frame % 10) == 0)
            printf("[bringup] frame=%lu led=%d\n", (unsigned long)frame, phase);
        frame++;
        sleep_ms(40);
    }
}
