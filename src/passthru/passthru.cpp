//
// DualSense -> Xbox passthrough, v1: TRANSPARENT GIP RELAY.
//
//   Xbox console  <--native USB (device, GIP)-->  Pico  <--PIO-USB (host)-->  real controller
//
// Every GIP packet the console sends is forwarded verbatim to the controller,
// and every packet the controller sends is forwarded back to the console. That
// includes the XSM3 auth (0x06) — the genuine controller's chip answers it, we
// just relay. This proves the auth relay end-to-end using the real controller's
// own input. Once this is accepted by the console, v2 substitutes DualSense
// input for the controller's INPUT (0x20) reports.
//
// Cross-core: device runs on core0 (tud), host on core1 (tuh). Two SPSC ring
// buffers carry framed packets between cores.
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
#include "pico/cyw43_arch.h"
#include "ssd1306.h"
#include "gip_host.h"
#include "ds_to_gip.h"
#include "bt.h"

// bt.cpp references these (declared in usb.h). We don't use the audio device,
// so provide the symbols directly instead of linking usb.cpp.
uint8_t mute[2]   = {0, 0};
float   volume[2] = {1.0f, 1.0f};

#define I2C_PORT   i2c1
#define PIN_SDA    10
#define PIN_SCL    11
#define OLED_ADDR  0x3C
#define OLED_H     32
#define PIN_USB_DP 2

static inline uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

//--------------------------------------------------------------------+
// Single-producer / single-consumer ring of framed packets: [u16 len][bytes]
//--------------------------------------------------------------------+
#define RING_SZ 4096
typedef struct {
    uint8_t buf[RING_SZ];
    volatile uint32_t head; // producer writes
    volatile uint32_t tail; // consumer reads
} ring_t;

static ring_t g_c2h; // console -> controller
static ring_t g_h2c; // controller -> console

static inline uint32_t ring_used(const ring_t *r) { return r->head - r->tail; }

static bool ring_push(ring_t *r, const uint8_t *data, uint16_t len) {
    if (len == 0 || len > 64) return false;
    if (RING_SZ - ring_used(r) < (uint32_t) (len + 2)) return false; // full
    uint32_t h = r->head;
    r->buf[h++ % RING_SZ] = (uint8_t) (len & 0xFF);
    r->buf[h++ % RING_SZ] = (uint8_t) (len >> 8);
    for (uint16_t i = 0; i < len; i++) r->buf[h++ % RING_SZ] = data[i];
    __dmb();
    r->head = h;
    return true;
}

// Returns packet length (0 if empty). Copies up to max bytes into out.
static uint16_t ring_pop(ring_t *r, uint8_t *out, uint16_t max) {
    if (ring_used(r) < 2) return 0;
    uint32_t t = r->tail;
    uint16_t len = (uint16_t) (r->buf[t % RING_SZ] | (r->buf[(t + 1) % RING_SZ] << 8));
    if (ring_used(r) < (uint32_t) (len + 2)) return 0; // partial (shouldn't happen)
    t += 2;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = r->buf[t++ % RING_SZ];
        if (i < max) out[i] = b;
    }
    __dmb();
    r->tail = t;
    return len;
}

static volatile uint32_t g_n_c2h = 0, g_n_h2c = 0;
static volatile bool g_ctrl_up = false, g_console_up = false;

// v2: DualSense input substitution. When a DualSense is linked over BT, its
// state fills g_ds and g_ds_valid=true; the relay then replaces the controller's
// INPUT (0x20) payload with DualSense-derived input, while still relaying the
// controller's auth/announce/identify verbatim. Off until BT is wired in.
static uint8_t g_ds[63];
static volatile bool g_ds_valid = false;
static volatile uint32_t g_ds_last_ms = 0;
static volatile uint32_t g_ds_reports = 0;

// DualSense report arrives over BT (same framing the original firmware uses:
// interrupt channel, report id 0x31, body at data+3).
static void bt_cb(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    if (channel == INTERRUPT && len >= 66 && data[1] == 0x31) {
        memcpy(g_ds, data + 3, 63);
        g_ds_valid = true;
        g_ds_last_ms = to_ms_since_boot(get_absolute_time());
        g_ds_reports++;
    }
}

//--------------------------------------------------------------------+
// Controller -> console  (host side, core1 context)
//--------------------------------------------------------------------+
extern "C" void gip_host_rx(const uint8_t *data, uint16_t len) {
    ring_push(&g_h2c, data, len);
    g_n_h2c++;
}
extern "C" void gip_host_mounted(uint8_t daddr) { (void) daddr; g_ctrl_up = true; }

//--------------------------------------------------------------------+
// Console -> controller  (device side, core0 context)
//--------------------------------------------------------------------+
extern "C" void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint32_t bufsize) {
    (void) itf; (void) buffer; (void) bufsize;
    uint8_t pkt[64];
    uint32_t n = tud_vendor_read(pkt, sizeof pkt);
    if (n) { ring_push(&g_c2h, pkt, (uint16_t) n); g_n_c2h++; }
}
extern "C" void tud_mount_cb(void)   { g_console_up = true; }
extern "C" void tud_umount_cb(void)  { g_console_up = false; }

//--------------------------------------------------------------------+
// core1: PIO-USB host + drain console->controller ring to the controller
//--------------------------------------------------------------------+
static void core1_main() {
    sleep_ms(10);
    pio_usb_configuration_t pcfg = PIO_USB_DEFAULT_CONFIG;
    pcfg.pin_dp = PIN_USB_DP;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pcfg);
    gip_host_set_relay(true);            // console drives GIP init; we don't inject power-on
    tuh_init(BOARD_TUH_RHPORT);

    uint8_t pkt[64];
    while (true) {
        tuh_task();
        if (gip_host_ready()) {
            uint16_t n = ring_pop(&g_c2h, pkt, sizeof pkt);
            if (n) gip_host_send_raw(pkt, n);
        }
    }
}

int main() {
    set_sys_clock_khz(120000, true);

    // DEVICE stack (to console) on core0.
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    stdio_init_all();
    sleep_ms(200);
    printf("\n[pt] passthrough relay @120MHz  console<->pico<->controller\n");

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA); gpio_pull_up(PIN_SCL);
    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();

    // Bluetooth (DualSense) on core0 — init before core1 so PIO/DMA are claimed first.
    bool bt_ok = (cyw43_arch_init() == 0);
    if (bt_ok) { bt_init(); bt_register_data_callback(bt_cb); }
    printf("[pt] cyw43/bt init: %s\n", bt_ok ? "ok" : "FAILED");

    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    uint8_t pkt[64];
    uint32_t last_hb = 0;
    char l0[22], l1[22], l2[22];
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'b' || c == 'B') { printf("[pt] BOOTSEL\n"); sleep_ms(20); reset_usb_boot(0, 0); }

        tud_task();
        cyw43_arch_poll();               // service Bluetooth (DualSense)

        uint32_t now0 = to_ms_since_boot(get_absolute_time());
        bool ds_live = g_ds_valid && (now0 - g_ds_last_ms) < 500;

        // Drain controller->console ring to the console.
        if (tud_vendor_mounted()) {
            uint16_t n = ring_pop(&g_h2c, pkt, sizeof pkt);
            if (n) {
                // v2: substitute live DualSense input for the controller's INPUT report.
                if (ds_live && pkt[0] == 0x20 && n >= 4 + 14) {
                    dualsense_to_gip_input(g_ds, pkt + 4);
                }
                tud_vendor_write(pkt, n);
                tud_vendor_write_flush();
            }
        }

        uint32_t now = now_ms();
        if (now - last_hb > 1000) {
            last_hb = now;
            printf("[pt] hb console=%d ctrl=%d ds=%d c2h=%lu h2c=%lu dsrpt=%lu\n",
                   g_console_up, g_ctrl_up, ds_live, (unsigned long) g_n_c2h,
                   (unsigned long) g_n_h2c, (unsigned long) g_ds_reports);
        }
        if (oled_ok) {
            // Line 0: link status of all three ends (Xbox host, USB controller, DualSense BT)
            snprintf(l0, sizeof l0, "XB%s CT%s DS%s", g_console_up ? "+" : "-",
                     g_ctrl_up ? "+" : "-", ds_live ? "+" : "-");
            // DualSense detail: battery %% (report byte ~52) + report count
            int batt = ds_live ? ((g_ds[52] & 0x0F) * 10) : 0;
            if (batt > 100) batt = 100;
            snprintf(l1, sizeof l1, "DS bat%d%% r%lu", batt, (unsigned long) g_ds_reports);
            // Live DualSense sticks/trigger to prove input is flowing
            snprintf(l2, sizeof l2, "LX%3u LY%3u L2%3u",
                     ds_live ? g_ds[0] : 0, ds_live ? g_ds[1] : 0, ds_live ? g_ds[4] : 0);
            oled.clear(false);
            oled.draw_text(2, 1, l0, 1);
            oled.draw_text(2, 12, l1, 1);
            oled.draw_text(2, 23, l2, 1);
            oled.show();
        }
    }
}
