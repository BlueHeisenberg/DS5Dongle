//
// GIP probe: enumerate a USB device on the PIO-USB host port, then dump its
// descriptors. Summary on the OLED; full hex over UART (115200, GP0/GP1).
//
// Purpose: capture the genuine Xbox Series controller's descriptors + endpoints
// so we can clone them for the device side. Works with ANY USB device today
// (e.g. the T1S dongle) to validate the descriptor-reading path.
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

#define I2C_PORT    i2c1
#define PIN_SDA     10
#define PIN_SCL     11
#define OLED_ADDR   0x3C
#define OLED_H      32
#define PIN_USB_DP  2

// Summary shared core1 -> core0 (racey but fine for a status display).
static volatile bool g_ready = false;
static volatile bool g_present = false;
static char g_l[4][22];

static tusb_desc_device_t s_dev;
static uint8_t s_cfg[512];

static void hexdump(const char *tag, const uint8_t *p, int n) {
    printf("[probe] %s (%d bytes):\n", tag, n);
    for (int i = 0; i < n; i++) {
        printf("%02X ", p[i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n");
}

// Parse the config descriptor: log interfaces/endpoints, build the OLED summary.
static void parse_config(uint16_t total) {
    hexdump("CONFIG DESC", s_cfg, total);

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(1, &vid, &pid);

    int n_if = 0, first_class = -1;
    int ep_in = -1, ep_out = -1, ep_in_mps = 0, ep_out_mps = 0;

    const uint8_t *p = s_cfg;
    const uint8_t *end = s_cfg + total;
    while (p + 2 <= end) {
        uint8_t len = p[0], type = p[1];
        if (len == 0) break;
        if (type == TUSB_DESC_INTERFACE) {
            n_if++;
            if (first_class < 0) first_class = p[5];      // bInterfaceClass
        } else if (type == TUSB_DESC_ENDPOINT) {
            uint8_t addr = p[2];
            int mps = p[4] | (p[5] << 8);
            if (addr & 0x80) { if (ep_in < 0)  { ep_in = addr;  ep_in_mps = mps; } }
            else             { if (ep_out < 0) { ep_out = addr; ep_out_mps = mps; } }
        }
        p += len;
    }

    printf("[probe] VID=%04X PID=%04X class=%u ifs=%d IN=0x%02X(%d) OUT=0x%02X(%d)\n",
           vid, pid, first_class, n_if, ep_in, ep_in_mps, ep_out, ep_out_mps);

    snprintf(g_l[0], sizeof g_l[0], "%04X:%04X", vid, pid);
    snprintf(g_l[1], sizeof g_l[1], "CLS%d IF%d", first_class, n_if);
    snprintf(g_l[2], sizeof g_l[2], "IN  %02X x%d", ep_in & 0xFF, ep_in_mps);
    snprintf(g_l[3], sizeof g_l[3], "OUT %02X x%d", ep_out & 0xFF, ep_out_mps);
    g_ready = true;
}

static void cfg_cb(tuh_xfer_t *xfer) {
    if (xfer->result == XFER_RESULT_SUCCESS) {
        uint16_t total = s_cfg[2] | (s_cfg[3] << 8);
        if (total > sizeof(s_cfg)) total = sizeof(s_cfg);
        parse_config(total);
    } else {
        printf("[probe] config descriptor fetch failed (%d)\n", xfer->result);
    }
}

static void dev_cb(tuh_xfer_t *xfer) {
    if (xfer->result == XFER_RESULT_SUCCESS) {
        hexdump("DEVICE DESC", (uint8_t *) &s_dev, sizeof(s_dev));
        tuh_descriptor_get_configuration(1, 0, s_cfg, sizeof(s_cfg), cfg_cb, 0);
    } else {
        printf("[probe] device descriptor fetch failed (%d)\n", xfer->result);
    }
}

extern "C" void tuh_mount_cb(uint8_t daddr) {
    printf("[probe] MOUNT addr=%u — fetching descriptors\n", daddr);
    g_present = true; g_ready = false;
    tuh_descriptor_get_device(daddr, &s_dev, sizeof(s_dev), dev_cb, 0);
}

extern "C" void tuh_umount_cb(uint8_t daddr) {
    (void) daddr;
    g_present = false; g_ready = false;
    printf("[probe] UNMOUNT\n");
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
    sleep_ms(400);
    printf("\n[probe] GIP descriptor probe @120MHz  D+=GP%d\n", PIN_USB_DP);

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();

    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    const char spin[4] = {'|', '/', '-', '\\'};
    uint32_t frame = 0;
    while (true) {
        if (oled_ok) {
            oled.clear(false);
            if (g_ready) {
                for (int i = 0; i < 4; i++) oled.draw_text(2, i * 8, g_l[i], 1);
            } else if (g_present) {
                oled.draw_text(2, 12, "READING...", 1);
            } else {
                char w[16]; snprintf(w, sizeof w, "GIP PROBE %c", spin[frame & 3]);
                oled.draw_text(2, 12, w, 1);
            }
            oled.show();
        }
        frame++;
        sleep_ms(150);
    }
}
