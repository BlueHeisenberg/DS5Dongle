//
// PIO-USB HOST bring-up test.
// Enumerates whatever USB device is plugged into the GP2/GP3 host port and
// shows its VID/PID on the OLED. Proves the donor-controller port works,
// using any Full-Speed device (a USB-serial adapter, the T1S dongle, a mouse...).
//
// Layout:
//   core1 = PIO-USB host stack (timing-critical, nothing else)
//   core0 = OLED status readout
//
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "pio_usb.h"
#include "tusb.h"
#include "ssd1306.h"

// ---- OLED (I2C1, GP10/GP11) ----
#define I2C_PORT    i2c1
#define PIN_SDA     10
#define PIN_SCL     11
#define OLED_ADDR   0x3C
#define OLED_H      32          // 0.91" 128x32 panel

// ---- PIO-USB host (D+ = GP2, D- = GP3) ----
#define PIN_USB_DP  2

// Shared state (written on core1 in the tuh callbacks, read on core0).
static volatile bool     g_mounted = false;
static volatile uint16_t g_vid = 0, g_pid = 0;
static volatile uint8_t  g_daddr = 0;
static volatile uint32_t g_mounts = 0;

extern "C" void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    g_vid = vid; g_pid = pid; g_daddr = daddr;
    g_mounted = true; g_mounts++;
    printf("[host] MOUNT addr=%u VID=%04X PID=%04X\n", daddr, vid, pid);
}

extern "C" void tuh_umount_cb(uint8_t daddr) {
    g_mounted = false;
    printf("[host] UNMOUNT addr=%u\n", daddr);
}

// core1: run the PIO-USB host stack and nothing else.
static void core1_main() {
    sleep_ms(10);
    pio_usb_configuration_t pcfg = PIO_USB_DEFAULT_CONFIG;
    pcfg.pin_dp = PIN_USB_DP;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pcfg);
    tuh_init(BOARD_TUH_RHPORT);
    while (true) {
        tuh_task();
    }
}

int main() {
    // Pico-PIO-USB requires a 120 MHz system clock.
    set_sys_clock_khz(120000, true);

    stdio_init_all();
    sleep_ms(400);
    printf("\n[host] PIO-USB host test @120MHz  D+=GP%d  D-=GP%d\n",
           PIN_USB_DP, PIN_USB_DP + 1);

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();
    printf("[host] OLED @0x%02X: %s\n", OLED_ADDR, oled_ok ? "ok" : "NO ACK");

    // Launch the host stack on core1.
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    const char spin[4] = {'|', '/', '-', '\\'};
    char top[12], bot[12];
    uint32_t frame = 0;

    while (true) {
        if (oled_ok) {
            oled.clear(false);
            if (g_mounted) {
                snprintf(top, sizeof top, "FOUND A%u", (unsigned) g_daddr);
                snprintf(bot, sizeof bot, "%04X %04X", g_vid, g_pid);
            } else {
                snprintf(top, sizeof top, "USB HOST");
                snprintf(bot, sizeof bot, "WAIT %c", spin[frame & 3]);
            }
            oled.draw_text(2, 1, top, 2);   // big: 2x 5x7 = 10x14
            oled.draw_text(2, 17, bot, 2);
            oled.show();
        }
        frame++;
        sleep_ms(150);
    }
}
