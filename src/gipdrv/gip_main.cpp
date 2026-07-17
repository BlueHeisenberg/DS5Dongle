//
// GIP host driver test: use the custom class driver to finally read the
// controller. Logs every packet it sends (over COM12), and requests IDENTIFY
// after the announce. If this works, we get the capability descriptor stream.
//
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "pio_usb.h"
#include "tusb.h"
#include "ssd1306.h"
#include "gip_host.h"

#define I2C_PORT   i2c1
#define PIN_SDA    10
#define PIN_SCL    11
#define OLED_ADDR  0x3C
#define OLED_H     32
#define PIN_USB_DP 2

static inline uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

static volatile uint32_t g_pkts = 0;
static volatile bool g_mounted = false;
static uint8_t g_last[8];
static volatile uint16_t g_last_len = 0;
static bool g_id_requested = false;

// Called from the class driver (core1 tuh context) for each GIP packet in.
extern "C" void gip_host_rx(const uint8_t *data, uint16_t len) {
    g_pkts++;
    uint16_t n = len < sizeof(g_last) ? len : sizeof(g_last);
    memcpy(g_last, data, n);
    g_last_len = len;
    printf("[gip] RX len=%u:", len);
    for (uint16_t i = 0; i < len && i < 40; i++) printf(" %02X", data[i]);
    printf("\n");

    // After the controller announces (0x02), ask it to IDENTIFY once.
    if (!g_id_requested && len > 0 && data[0] == 0x02) {
        g_id_requested = true;
        gip_host_send(0x04, 0x20, NULL, 0);
        printf("[gip] -> identify request\n");
    }
}

extern "C" void gip_host_mounted(uint8_t daddr) {
    g_mounted = true;
    printf("[gip] MOUNTED addr=%u, powered on, polling IN\n", daddr);
}

static void core1_main() {
    sleep_ms(10);
    pio_usb_configuration_t pcfg = PIO_USB_DEFAULT_CONFIG;
    pcfg.pin_dp = PIN_USB_DP;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pcfg);
    tuh_init(BOARD_TUH_RHPORT);
    while (true) tuh_task();
}

int main() {
    set_sys_clock_khz(120000, true);
    stdio_init_all();
    sleep_ms(300);
    printf("\n[gip] GIP host-driver test @120MHz DP=GP%d\n", PIN_USB_DP);

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA); gpio_pull_up(PIN_SCL);
    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();

    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    uint32_t last_hb = 0;
    char l0[22], l1[22], l2[22];
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'b' || c == 'B') { printf("[gip] BOOTSEL\n"); sleep_ms(20); reset_usb_boot(0, 0); }

        uint32_t now = now_ms();
        if (now - last_hb > 1000) {
            last_hb = now;
            printf("[gip] hb mounted=%d pkts=%lu\n", g_mounted, (unsigned long) g_pkts);
        }
        if (oled_ok) {
            oled.clear(false);
            snprintf(l0, sizeof l0, "GIP DRV %s", g_mounted ? "UP" : "..");
            snprintf(l1, sizeof l1, "RX %lu pkt", (unsigned long) g_pkts);
            if (g_last_len) snprintf(l2, sizeof l2, "%02X %02X %02X len%u", g_last[0], g_last[1], g_last[2], g_last_len);
            else snprintf(l2, sizeof l2, "no data yet");
            oled.draw_text(2, 0, l0, 1);
            oled.draw_text(2, 11, l1, 1);
            oled.draw_text(2, 22, l2, 1);
            oled.show();
        }
        sleep_ms(80);
    }
}
