//
// Xbox-controller DEVICE spoof — Milestone 3: answer IDENTIFY (experiment).
//
// Windows engages our announce, then requests IDENTIFY (0x04). Here we reply with
// a MINIMAL, single-transfer identify descriptor (header + one firmware entry, no
// chunking, no classes yet). Goal: find out whether Windows accepts our identify
// framing (stops retrying 0x04 and advances) or rejects it. We capture cmd+opt of
// every host packet so we can see what it does next (especially a 0x06 auth).
//
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "ssd1306.h"

#define I2C_PORT   i2c1
#define PIN_SDA    10
#define PIN_SCL    11
#define OLED_ADDR  0x3C
#define OLED_H     32

#define GIP_OPT_ACK       0x10
#define GIP_OPT_INTERNAL  0x20

static inline uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

static uint32_t g_rx_packets = 0;
static uint8_t  g_seq = 1;
static volatile bool g_want_identify = false;
static uint32_t g_identify_sent = 0;
static volatile bool g_powered = false;
static uint8_t  g_pwr_mode = 0xEE;   // captured power-mode byte

// Capture cmd+opt of the first host packets to read the sequence off the OLED.
#define CAP_MAX 10
static uint8_t g_cap_cmd[CAP_MAX];
static uint8_t g_cap_opt[CAP_MAX];
static int     g_ncap = 0;

static bool gip_send(uint8_t cmd, uint8_t opt, const uint8_t *payload, uint8_t len) {
    if (!tud_vendor_mounted()) return false;
    uint8_t pkt[4 + 64];
    pkt[0] = cmd; pkt[1] = opt; pkt[2] = g_seq++; pkt[3] = len;
    if (len) memcpy(pkt + 4, payload, len);
    uint32_t n = tud_vendor_write(pkt, 4 + len);
    tud_vendor_write_flush();
    return n > 0;
}

// ANNOUNCE (28B): address[6], unknown[2], vid, pid, fw_ver[8], hw_ver[8]
static void send_announce() {
    uint8_t p[28];
    memset(p, 0, sizeof p);
    const uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(p, mac, 6);
    p[8] = 0x5E; p[9] = 0x04;   // vid 0x045E
    p[10] = 0xEA; p[11] = 0x02; // pid 0x02EA
    p[12] = 0x01; p[20] = 0x01; // fw/hw major = 1
    gip_send(0x02, GIP_OPT_INTERNAL, p, sizeof p);
    printf("[xdev] announce sent\n");
}

// Minimal IDENTIFY response (fits one 64B transfer, no chunking yet).
// Header: unknown[16] + 8x LE16 offsets. Only firmware_versions present.
static void send_identify() {
    uint8_t p[64];
    memset(p, 0, sizeof p);
    // offsets start at p[16]; order: client_cmds, fw, audio, cap_out, cap_in, classes, ifaces, hid
    // firmware_versions_offset (index 1) -> 32
    p[16 + 2] = 32; p[16 + 3] = 0;
    // firmware_versions section @32: count=1, {major=5, minor=0} (LE16 each)
    p[32] = 1;
    p[33] = 5; p[34] = 0;   // major
    p[35] = 0; p[36] = 0;   // minor
    gip_send(0x04, GIP_OPT_INTERNAL, p, 37);
    g_identify_sent++;
    printf("[xdev] identify sent (#%lu)\n", (unsigned long) g_identify_sent);
}

// STATUS (0x03): status byte + 3 unknown
static void send_status() {
    uint8_t p[4] = {0x00, 0x00, 0x00, 0x00};
    gip_send(0x03, GIP_OPT_INTERNAL, p, sizeof p);
}

// Standard gamepad INPUT (0x20), 14B: buttons(u16), triggers(2xu16), sticks(4xs16)
static void send_input(bool a) {
    uint8_t p[14];
    memset(p, 0, sizeof p);
    uint16_t b = a ? (1u << 4) : 0;
    p[0] = b & 0xFF; p[1] = b >> 8;
    gip_send(0x20, 0x00, p, sizeof p);
}

extern "C" void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint32_t bufsize) {
    (void) itf; (void) buffer; (void) bufsize;
    uint8_t pkt[64];
    uint32_t n = tud_vendor_read(pkt, sizeof pkt);
    if (n == 0) return;
    g_rx_packets++;
    if (g_ncap < CAP_MAX) {
        g_cap_cmd[g_ncap] = pkt[0];
        g_cap_opt[g_ncap] = n > 1 ? pkt[1] : 0;
        g_ncap++;
    }
    if (pkt[0] == 0x04) g_want_identify = true;          // IDENTIFY request
    if (pkt[0] == 0x05) { g_powered = true; if (n > 4) g_pwr_mode = pkt[4]; } // POWER
    printf("[xdev] RX %lu:", (unsigned long) n);
    for (uint32_t i = 0; i < n; i++) printf(" %02X", pkt[i]);
    printf("\n");
}

static bool g_announced = false;
static uint32_t g_mount_ms = 0;

extern "C" void tud_mount_cb(void)  { g_announced = false; g_mount_ms = now_ms(); }
extern "C" void tud_umount_cb(void) { g_announced = false; }

int main() {
    board_init();
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    stdio_init_all();
    printf("\n[xdev] Xbox spoof — answer IDENTIFY experiment\n");

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    SSD1306 oled(I2C_PORT, OLED_ADDR, OLED_H);
    bool oled_ok = oled.init();

    uint32_t last_draw = 0;
    char l0[20], l1[20], l2[20], l3[20];

    while (true) {
        tud_task();
        uint32_t now = now_ms();

        if (tud_mounted() && !g_announced && (now - g_mount_ms) > 250) {
            send_announce();
            g_announced = true;
        }
        if (g_want_identify) {
            g_want_identify = false;
            send_identify();
        }
        // Once Windows powers us on: send status, then stream input at ~50Hz.
        static bool status_done = false;
        static uint32_t last_in = 0;
        if (g_powered && !status_done) { send_status(); status_done = true; }
        if (g_powered && (now - last_in) >= 20) {
            last_in = now;
            send_input(((now / 1000) & 1) != 0); // A toggles each second
        }

        if (oled_ok && (now - last_draw) >= 120) {
            last_draw = now;
            oled.clear(false);
            snprintf(l0, sizeof l0, "N%d RX%lu ID%lu P%02X", g_ncap, (unsigned long) g_rx_packets,
                     (unsigned long) g_identify_sent, g_pwr_mode);
            char *lines[3] = {l1, l2, l3};
            for (int r = 0; r < 3; r++) {
                int i = r * 3;
                lines[r][0] = '\0';
                int off = 0;
                for (int k = 0; k < 3 && (i + k) < g_ncap; k++)
                    off += snprintf(lines[r] + off, 20 - off, "%02X:%02X ",
                                    g_cap_cmd[i + k], g_cap_opt[i + k]);
            }
            oled.draw_text(2, 0, l0, 1);
            oled.draw_text(2, 8, l1, 1);
            oled.draw_text(2, 16, l2, 1);
            oled.draw_text(2, 24, l3, 1);
            oled.show();
        }
    }
}
