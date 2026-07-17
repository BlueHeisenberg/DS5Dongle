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
static volatile bool g_host_ready = false; // core1 sets after PIO-USB host init

// v2: DualSense input substitution. When a DualSense is linked over BT, its
// state fills g_ds and g_ds_valid=true; the relay then replaces the controller's
// INPUT (0x20) payload with DualSense-derived input, while still relaying the
// controller's auth/announce/identify verbatim. Off until BT is wired in.
static uint8_t g_ds[63];
static volatile bool g_ds_valid = false;
static volatile bool g_ds_dirty = false;   // new DualSense state to push to console
static volatile uint32_t g_ds_last_ms = 0;
static volatile uint32_t g_ds_reports = 0;
static uint8_t g_in_seq = 0;                // sequence for our injected input reports

// DualSense report arrives over BT (same framing the original firmware uses:
// interrupt channel, report id 0x31, body at data+3).
static void bt_cb(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    if (channel == INTERRUPT && len >= 66 && data[1] == 0x31) {
        memcpy(g_ds, data + 3, 63);
        g_ds_valid = true;
        g_ds_dirty = true;
        g_ds_last_ms = to_ms_since_boot(get_absolute_time());
        g_ds_reports++;
    }
}

// Generate a GIP INPUT report (0x20) directly from the DualSense state and send
// it to the console. Decoupled from the controller's carrier so input flows at
// the DualSense's rate regardless of whether the donor controller streams.
static void send_input_to_console() {
    uint8_t pkt[4 + 14];
    pkt[0] = 0x20; pkt[1] = 0x00; pkt[2] = g_in_seq++; pkt[3] = 14;
    dualsense_to_gip_input(g_ds, pkt + 4);
    tud_vendor_write(pkt, sizeof pkt);
    tud_vendor_write_flush();
}

//--------------------------------------------------------------------+
// Controller -> console  (host side, core1 context)
//--------------------------------------------------------------------+
static volatile uint32_t g_ctrl_in = 0; // controller INPUT (0x20) count, for rate calc
extern "C" void gip_host_rx(const uint8_t *data, uint16_t len) {
    ring_push(&g_h2c, data, len);
    g_n_h2c++;
    if (len > 0 && data[0] == 0x20) g_ctrl_in++;
}
extern "C" void gip_host_mounted(uint8_t daddr) { (void) daddr; g_ctrl_up = true; }

//--------------------------------------------------------------------+
// Rumble / adaptive-trigger feedback: GIP rumble (0x09) -> DualSense output.
// Builds a DualSense 0x31 output report (same framing the original firmware
// uses) and sends it over the BT link. Main motors map to DualSense rumble;
// the Xbox impulse-trigger levels drive a DualSense trigger-vibration effect.
//--------------------------------------------------------------------+
static void ds_send_feedback(uint8_t motorL, uint8_t motorR, uint8_t trigL, uint8_t trigR) {
    static uint8_t seq = 0;
    uint8_t out[78];
    memset(out, 0, sizeof out);
    out[0] = 0x31;               // DualSense output report id
    out[1] = (uint8_t) (seq++ << 4);
    out[2] = 0x10;
    uint8_t *b = out + 3;        // SetStateData
    b[0] = 0x01 | 0x04 | 0x08;   // enable: rumble + right/left trigger effects
    b[2] = motorR;               // right motor (high-freq)
    b[3] = motorL;               // left motor (low-freq)
    // Adaptive-trigger "vibration" effect from the impulse-trigger levels.
    // Offsets per the common DualSense output layout; effect tuning is easy to adjust.
    if (trigR) { b[11] = 0x26; b[12] = 0x90; b[13] = trigR; }  // right trigger
    if (trigL) { b[22] = 0x26; b[23] = 0x90; b[24] = trigL; }  // left trigger
    bt_write(out, sizeof out);
}

//--------------------------------------------------------------------+
// Console -> controller  (device side, core0 context)
//--------------------------------------------------------------------+
extern "C" void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint32_t bufsize) {
    (void) itf; (void) buffer; (void) bufsize;
    uint8_t pkt[64];
    uint32_t n = tud_vendor_read(pkt, sizeof pkt);
    if (!n) return;

    // Relay the packet to the controller.
    ring_push(&g_c2h, pkt, (uint16_t) n);
    g_n_c2h++;

    // GIP rumble (0x09): also drive the DualSense. Payload (after 4B header):
    //   [0] unknown [1] motors [2] left_trigger [3] right_trigger [4] left [5] right
    if (n >= 10 && pkt[0] == 0x09 && g_ds_valid) {
        uint8_t tL = pkt[6], tR = pkt[7], mL = pkt[8], mR = pkt[9];
        static uint8_t last[4]; static uint32_t last_ms;
        uint32_t nowr = to_ms_since_boot(get_absolute_time());
        if (mL != last[0] || mR != last[1] || tL != last[2] || tR != last[3] || nowr - last_ms > 200) {
            last[0] = mL; last[1] = mR; last[2] = tL; last[3] = tR; last_ms = nowr;
            ds_send_feedback(mL, mR, tL, tR);
        }
    }
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
    // CYW43 (Bluetooth) dynamically claims PIO0; put PIO-USB on PIO1 so their
    // programs don't collide in PIO0's 32-instruction memory.
    pcfg.pio_tx_num = 1;
    pcfg.pio_rx_num = 1;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pcfg);
    // Host drives the controller on (power-on) so it always streams its input
    // carrier — otherwise on a lenient host (PC) it stays idle and no input flows.
    gip_host_set_relay(false);
    tuh_init(BOARD_TUH_RHPORT);
    g_host_ready = true;                 // let core0 bring up CYW43 only after PIO-USB owns its resources

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

    // Bring up PIO-USB host (core1) FIRST so it claims its PIO/DMA, then start
    // CYW43/Bluetooth which claims free resources around it.
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    uint32_t t_wait = to_ms_since_boot(get_absolute_time());
    while (!g_host_ready && (to_ms_since_boot(get_absolute_time()) - t_wait) < 3000) tight_loop_contents();

    bool bt_ok = (cyw43_arch_init() == 0);
    if (bt_ok) { bt_init(); bt_register_data_callback(bt_cb); }
    printf("[pt] host_ready=%d  cyw43/bt init: %s\n", g_host_ready, bt_ok ? "ok" : "FAILED");

    uint8_t pkt[64];
    uint32_t last_hb = 0, last_oled = 0;
    char l0[22], l1[22], l2[22], l3[22];
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == 'b' || c == 'B') { printf("[pt] BOOTSEL\n"); sleep_ms(20); reset_usb_boot(0, 0); }

        tud_task();
        cyw43_arch_poll();               // service Bluetooth (DualSense)

        uint32_t now0 = to_ms_since_boot(get_absolute_time());
        bool ds_live = g_ds_valid && (now0 - g_ds_last_ms) < 500;

        // Inject our own input report to the console the instant new DualSense
        // state arrives (decoupled from the controller carrier — lowest latency).
        if (tud_vendor_mounted() && ds_live && g_ds_dirty) {
            g_ds_dirty = false;
            send_input_to_console();
        }

        // Relay controller->console, but DROP the controller's own input (0x20) —
        // we generate input from the DualSense. Everything else (announce/identify/
        // auth/status) is forwarded verbatim.
        if (tud_vendor_mounted()) {
            uint16_t n;
            while ((n = ring_pop(&g_h2c, pkt, sizeof pkt)) != 0) {
                if (pkt[0] == 0x20) continue;      // controller input not used
                tud_vendor_write(pkt, n);
                tud_vendor_write_flush();
            }
        }

        uint32_t now = now_ms();
        if (now - last_hb > 1000) {
            uint32_t dt = now - last_hb; last_hb = now;
            static uint32_t p_ds = 0, p_ctrl = 0;
            uint32_t ds_hz   = (g_ds_reports - p_ds) * 1000 / (dt ? dt : 1);
            uint32_t ctrl_hz = (g_ctrl_in    - p_ctrl) * 1000 / (dt ? dt : 1);
            p_ds = g_ds_reports; p_ctrl = g_ctrl_in;
            // Rates tell us the latency budget: DS report interval + controller
            // (input carrier) poll interval + 1ms device poll.
            printf("[pt] console=%d ctrl=%d ds=%d | DS %luHz (%lums)  CTRL %luHz (%lums)  loop-fast\n",
                   g_console_up, g_ctrl_up, ds_live,
                   (unsigned long) ds_hz,   (unsigned long) (ds_hz ? 1000 / ds_hz : 0),
                   (unsigned long) ctrl_hz, (unsigned long) (ctrl_hz ? 1000 / ctrl_hz : 0));
        }
        // Status panel — connection info only, 1 Hz (cheap; the ~12 ms blocking
        // I2C write stays well out of the relay hot path).
        if (oled_ok && now - last_oled > 1000) {
            last_oled = now;
            // Host (Xbox/PC) + donor Xbox controller (USB) presence.
            snprintf(l0, sizeof l0, "HOST%s  PAD%s",
                     g_console_up ? ":ok" : ":--", g_ctrl_up ? ":ok" : ":--");
            // DualSense state: LINK (streaming) / CONN (paired) / SRCH (looking) + battery.
            const char *dss = ds_live ? "LINK" : (bt_is_connected() ? "CONN" : "SRCH");
            int batt = ds_live ? ((g_ds[52] & 0x0F) * 10) : 0;
            if (batt > 100) batt = 100;
            snprintf(l1, sizeof l1, "DS:%s  BAT %d%%", dss, batt);
            // DualSense MAC (valid once found/paired).
            uint8_t a[6]; bt_get_addr(a);
            if (bt_is_connected() || ds_live)
                snprintf(l2, sizeof l2, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
            else
                snprintf(l2, sizeof l2, "MAC --:--:--:--");
            snprintf(l3, sizeof l3, "DualSense -> Xbox");
            oled.clear(false);
            oled.draw_text(2, 0, l0, 1);
            oled.draw_text(2, 8, l1, 1);
            oled.draw_text(2, 16, l2, 1);
            oled.draw_text(2, 24, l3, 1);
            oled.show();
        }
    }
}
