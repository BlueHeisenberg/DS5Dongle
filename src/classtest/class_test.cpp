//
// Class-driver test: prove PIO-USB IN transfers work using TinyUSB's own MSC
// (pendrive) and HID class drivers, instead of our raw-endpoint GIP code.
//   - Plug a USB pendrive -> we read block 0 (bulk IN). Success => IN path works.
//   - Plug any wired HID  -> we log its reports (interrupt IN).
// All output on UART (COM12, 115200).
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

#define I2C_PORT   i2c1
#define PIN_SDA    10
#define PIN_SCL    11
#define OLED_ADDR  0x3C
#define OLED_H     32
#define PIN_USB_DP 2

static inline uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

static volatile int  g_state = 0;   // 0 idle,1 msc mounted,2 read ok,3 read fail
static volatile int  g_hid_reports = 0;
static char g_line[24] = "waiting device";

static uint8_t msc_buf[512] __attribute__((aligned(4)));

// ---------- MSC (pendrive) ----------
static bool msc_read_done(uint8_t addr, tuh_msc_complete_data_t const *cb) {
    (void) addr;
    bool ok = (cb->csw->status == 0);
    printf("[cls] MSC read10 done ok=%d:", ok);
    for (int i = 0; i < 16; i++) printf(" %02X", msc_buf[i]);
    printf("\n");
    g_state = ok ? 2 : 3;
    snprintf(g_line, sizeof g_line, ok ? "MSC READ OK!" : "MSC READ FAIL");
    return true;
}

extern "C" void tuh_msc_mount_cb(uint8_t addr) {
    uint32_t bc = tuh_msc_get_block_count(addr, 0);
    uint32_t bs = tuh_msc_get_block_size(addr, 0);
    printf("[cls] MSC MOUNT addr=%u blocks=%lu bsize=%lu -> read LBA0\n",
           addr, (unsigned long) bc, (unsigned long) bs);
    g_state = 1;
    snprintf(g_line, sizeof g_line, "MSC rd %lublk", (unsigned long) bc);
    tuh_msc_read10(addr, 0, msc_buf, 0, 1, msc_read_done, 0);
}
extern "C" void tuh_msc_umount_cb(uint8_t addr) { (void) addr; printf("[cls] MSC UNMOUNT\n"); g_state = 0; }

// ---------- HID ----------
extern "C" void tuh_hid_mount_cb(uint8_t addr, uint8_t inst, uint8_t const *desc, uint16_t len) {
    (void) desc; (void) len;
    uint16_t vid = 0, pid = 0; tuh_vid_pid_get(addr, &vid, &pid);
    printf("[cls] HID MOUNT addr=%u inst=%u %04X:%04X\n", addr, inst, vid, pid);
    tuh_hid_receive_report(addr, inst);
}
extern "C" void tuh_hid_report_received_cb(uint8_t addr, uint8_t inst, uint8_t const *report, uint16_t len) {
    g_hid_reports++;
    printf("[cls] HID rpt inst=%u len=%u:", inst, len);
    for (uint16_t i = 0; i < len && i < 20; i++) printf(" %02X", report[i]);
    printf("\n");
    snprintf(g_line, sizeof g_line, "HID rpt #%d", g_hid_reports);
    tuh_hid_receive_report(addr, inst);
}

extern "C" void tuh_mount_cb(uint8_t addr) {
    uint16_t vid = 0, pid = 0; tuh_vid_pid_get(addr, &vid, &pid);
    printf("[cls] MOUNT addr=%u %04X:%04X\n", addr, vid, pid);
}
extern "C" void tuh_umount_cb(uint8_t addr) { (void) addr; printf("[cls] UNMOUNT\n"); }

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
    printf("\n[cls] class-driver test @120MHz DP=GP%d (MSC+HID)\n", PIN_USB_DP);

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA); gpio_pull_up(PIN_SCL);
    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();

    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    uint32_t last_hb = 0;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'b' || c == 'B') { printf("[cls] BOOTSEL\n"); sleep_ms(20); reset_usb_boot(0, 0); }

        uint32_t now = now_ms();
        if (now - last_hb > 1000) {
            last_hb = now;
            printf("[cls] hb state=%d hid=%d\n", g_state, g_hid_reports);
        }
        if (oled_ok) {
            oled.clear(false);
            oled.draw_text(2, 4, "CLASS TEST", 1);
            oled.draw_text(2, 18, g_line, 1);
            oled.show();
        }
        sleep_ms(80);
    }
}
