//
// GIP capture: act as GIP host to the genuine Xbox controller on the PIO-USB
// port, drive the init handshake (power-on -> announce -> identify request), and
// record every packet the controller sends into a flash region. Dump it later
// with:  picotool save -r 0x10200000 0x10200800 cap.bin
//
// Capture format in flash: "GIPC" magic, u16 total_len, then repeated
// [u8 pkt_len][pkt bytes...] framing so packet boundaries are preserved.
//
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pio_usb.h"
#include "tusb.h"
#include "ssd1306.h"

#define I2C_PORT    i2c1
#define PIN_SDA     10
#define PIN_SCL     11
#define OLED_ADDR   0x3C
#define OLED_H      32
#define PIN_USB_DP  2

#define GIP_OPT_INTERNAL 0x20
#define FLASH_OFF        0x200000              // 2 MiB in; well past the program
#define CAP_CAP          1536                  // captured-bytes capacity

// ---- shared state (core1 tuh context writes; core0 reads/flashes) ----
static uint8_t  g_daddr = 0;
static uint8_t  g_ep_in = 0, g_ep_out = 0;
static tusb_desc_endpoint_t g_din, g_dout;
static bool     g_opened = false;
static uint8_t  g_inbuf[64];
static uint8_t  g_cap[CAP_CAP];
static volatile uint16_t g_caplen = 0;
static volatile uint32_t g_npkt = 0;
static bool     g_sent_identify = false;
static volatile bool g_done = false;   // capture window closed -> flash it

// diagnostics
static volatile bool     g_open_in = false, g_open_out = false, g_pwr_ok = false, g_in_sub = false;
static volatile uint32_t g_incb = 0;   // IN completion callbacks (any result)
static volatile int      g_lastres = -1;
static volatile uint16_t g_lastlen = 0;

static uint8_t s_cfg[512];
static uint16_t g_cfg_len = 0;

static void cap_append(const uint8_t *p, uint32_t n) {
    if (n == 0 || n > 63) n = n > 63 ? 63 : n;
    if (g_caplen + 1 + n > CAP_CAP) { g_done = true; return; }
    g_cap[g_caplen++] = (uint8_t) n;
    memcpy(g_cap + g_caplen, p, n);
    g_caplen += n;
    g_npkt++;
}

static void out_done(tuh_xfer_t *x) { (void) x; }

static bool gip_out(uint8_t cmd, uint8_t opt, const uint8_t *pl, uint8_t len) {
    static uint8_t seq = 1;
    static uint8_t pkt[64];
    pkt[0] = cmd; pkt[1] = opt; pkt[2] = seq++; pkt[3] = len;
    if (len) memcpy(pkt + 4, pl, len);
    tuh_xfer_t x = {};
    x.daddr = g_daddr; x.ep_addr = g_ep_out;
    x.buffer = pkt; x.buflen = (uint16_t) (4 + len);
    x.complete_cb = out_done;
    return tuh_edpt_xfer(&x);
}

static bool submit_in();

static void in_done(tuh_xfer_t *x) {
    g_incb++;
    g_lastres = (int) x->result;
    g_lastlen = (uint16_t) x->actual_len;
    printf("[cap] IN cb res=%d len=%lu:", (int) x->result, (unsigned long) x->actual_len);
    for (uint32_t i = 0; i < x->actual_len && i < 32; i++) printf(" %02X", g_inbuf[i]);
    printf("\n");
    if (x->result == XFER_RESULT_SUCCESS && x->actual_len > 0) {
        cap_append(g_inbuf, x->actual_len);
        // After the controller's first packet (announce), request IDENTIFY once.
        if (!g_sent_identify) {
            g_sent_identify = true;
            gip_out(0x04, GIP_OPT_INTERNAL, NULL, 0); // IDENTIFY request (header only)
        }
    }
    if (!g_done) submit_in();
}

static bool submit_in() {
    tuh_xfer_t x = {};
    x.daddr = g_daddr; x.ep_addr = g_ep_in;
    x.buffer = g_inbuf; x.buflen = sizeof(g_inbuf);
    x.complete_cb = in_done;
    return tuh_edpt_xfer(&x);
}

static void cfg_cb(tuh_xfer_t *x) {
    if (x->result != XFER_RESULT_SUCCESS) return;
    uint16_t total = s_cfg[2] | (s_cfg[3] << 8);
    if (total > sizeof(s_cfg)) total = sizeof(s_cfg);
    g_cfg_len = total;
    const uint8_t *p = s_cfg, *end = s_cfg + total;
    while (p + 2 <= end) {
        uint8_t len = p[0], type = p[1];
        if (len == 0) break;
        if (type == TUSB_DESC_ENDPOINT) {
            uint8_t addr = p[2], attr = p[3];
            if ((attr & 0x03) == TUSB_XFER_INTERRUPT) {
                if ((addr & 0x80) && !g_ep_in)  { g_ep_in = addr;  memcpy(&g_din, p, 7); }
                if (!(addr & 0x80) && !g_ep_out) { g_ep_out = addr; memcpy(&g_dout, p, 7); }
            }
        }
        p += len;
    }
    printf("[cap] config len=%u ep_in=%02X ep_out=%02X\n", total, g_ep_in, g_ep_out);
    if (g_ep_in) {
        g_open_in = tuh_edpt_open(g_daddr, &g_din);
        if (g_ep_out) g_open_out = tuh_edpt_open(g_daddr, &g_dout);
        g_opened = true;
        g_in_sub = submit_in();                               // poll IN
        if (g_ep_out) {                                        // GIP power-on if there's an OUT
            uint8_t on = 0x00;
            g_pwr_ok = gip_out(0x05, GIP_OPT_INTERNAL, &on, 1);
        }
        printf("[cap] opened in=%d out=%d sub=%d pwr=%d\n", g_open_in, g_open_out, g_in_sub, g_pwr_ok);
    }
}

extern "C" void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    printf("[cap] MOUNT addr=%u %04X:%04X\n", daddr, vid, pid);
    g_daddr = daddr;
    g_ep_in = g_ep_out = 0;
    tuh_descriptor_get_configuration(daddr, 0, s_cfg, sizeof(s_cfg), cfg_cb, 0);
}
extern "C" void tuh_umount_cb(uint8_t daddr) {
    (void) daddr;
    printf("[cap] UNMOUNT (device removed)\n");
    g_opened = false; g_ep_in = 0; g_ep_out = 0;
    g_open_in = g_open_out = g_pwr_ok = g_in_sub = false;
}

static void core1_main() {
    sleep_ms(10);
    pio_usb_configuration_t pcfg = PIO_USB_DEFAULT_CONFIG;
    pcfg.pin_dp = PIN_USB_DP;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pcfg);
    tuh_init(BOARD_TUH_RHPORT);
    while (true) tuh_task();
}

static inline uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

static void flash_capture() {
    // Stop the PIO-USB host core so nothing executes from XIP during flash ops.
    multicore_reset_core1();
    sleep_ms(5);

    static uint8_t page[512];
    memset(page, 0xFF, sizeof page);
    page[0] = 'G'; page[1] = 'I'; page[2] = 'P'; page[3] = 'C';
    // Diagnostic header
    page[4]  = g_open_in;  page[5] = g_open_out; page[6] = g_pwr_ok; page[7] = g_in_sub;
    page[8]  = g_incb & 0xFF; page[9] = (g_incb >> 8) & 0xFF;
    page[10] = (int8_t) g_lastres; page[11] = g_lastlen & 0xFF;
    page[12] = g_ep_in; page[13] = g_ep_out;
    uint16_t n = g_caplen;
    page[14] = n & 0xFF; page[15] = n >> 8;
    page[16] = g_npkt & 0xFF; page[17] = (g_npkt >> 8) & 0xFF;
    // Full config descriptor at offset 32 (so we can read the real endpoint layout)
    page[18] = g_cfg_len & 0xFF; page[19] = (g_cfg_len >> 8) & 0xFF;
    uint16_t clen = g_cfg_len > 220 ? 220 : g_cfg_len;
    memcpy(page + 32, s_cfg, clen);
    // Captured packets after the config, at offset 256
    uint16_t room = sizeof(page) - 256;
    memcpy(page + 256, g_cap, n > room ? room : n);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_OFF, 4096);
    flash_range_program(FLASH_OFF, page, sizeof(page));
    restore_interrupts(ints);
}

int main() {
    set_sys_clock_khz(120000, true);
    stdio_init_all();
    sleep_ms(300);
    printf("\n[cap] GIP capture start @120MHz DP=GP%d\n", PIN_USB_DP);

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();

    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    uint32_t t_open = 0;
    bool flashed = false;
    int repwr = 0;
    char l0[22], l1[22], l2[22], l3[22];

    while (true) {
        uint32_t now = now_ms();

        // Serial trigger: send 'b' over the FTDI to drop into BOOTSEL (no button).
        int c = getchar_timeout_us(0);
        if (c == 'b' || c == 'B') { printf("[cap] rebooting to BOOTSEL\n"); sleep_ms(20); reset_usb_boot(0, 0); }

        if (g_opened && t_open == 0) t_open = now;

        // Re-send power-on a few times in the first ~1.5s (in case the first was dropped).
        if (g_opened && repwr < 5 && t_open && (now - t_open) > (uint32_t)(200 + repwr * 250)) {
            uint8_t on = 0x00;
            gip_out(0x05, GIP_OPT_INTERNAL, &on, 1);
            repwr++;
        }

        // Proactively request IDENTIFY ~600ms after power (in case it waits for us).
        static bool id_proactive = false;
        if (g_opened && !id_proactive && t_open && (now - t_open) > 600) {
            gip_out(0x04, GIP_OPT_INTERNAL, NULL, 0);
            id_proactive = true;
        }

        // Long 20s window so you can press buttons / the Xbox guide to wake it.
        if (g_opened && !g_done && (now - t_open) > 20000) g_done = true;
        if (g_caplen > CAP_CAP - 80) g_done = true; // buffer nearly full

        static uint32_t last_hb = 0;
        if (now - last_hb > 1000) {
            last_hb = now;
            printf("[cap] hb open=%d cb=%lu npkt=%lu cap=%u res=%d ep_in=%02X\n",
                   g_opened, (unsigned long) g_incb, (unsigned long) g_npkt,
                   g_caplen, g_lastres, g_ep_in);
        }

        if (g_done && !flashed) { flash_capture(); flashed = true; }

        if (oled_ok) {
            oled.clear(false);
            if (flashed) {
                snprintf(l0, sizeof l0, "CAPTURED %u B", g_caplen);
                snprintf(l1, sizeof l1, "%lu pkt", (unsigned long) g_npkt);
                snprintf(l2, sizeof l2, "BOOTSEL +");
                snprintf(l3, sizeof l3, "picotool save");
            } else if (g_opened) {
                snprintf(l0, sizeof l0, "EP I%02X O%02X op%d%d", g_ep_in, g_ep_out, g_open_in, g_open_out);
                snprintf(l1, sizeof l1, "pwr%d sub%d", g_pwr_ok, g_in_sub);
                snprintf(l2, sizeof l2, "cb%lu r%d l%u", (unsigned long) g_incb, g_lastres, g_lastlen);
                snprintf(l3, sizeof l3, "%uB %lupkt PRESS", g_caplen, (unsigned long) g_npkt);
            } else {
                snprintf(l0, sizeof l0, "GIP CAPTURE");
                snprintf(l1, sizeof l1, "waiting pad");
                l2[0] = l3[0] = '\0';
            }
            oled.draw_text(2, 0, l0, 1);
            oled.draw_text(2, 8, l1, 1);
            oled.draw_text(2, 16, l2, 1);
            oled.draw_text(2, 24, l3, 1);
            oled.show();
        }
        sleep_ms(100);
    }
}
