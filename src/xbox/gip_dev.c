//
// Custom GIP device class driver (registered via usbd_app_driver_get_cb).
// Claims the whole multi-interface GIP configuration so SET_CONFIGURATION
// succeeds, opens IF0's interrupt endpoints (the GIP data path), and leaves the
// audio(iso)/bulk endpoints declared-but-unopened.
//
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "gip_dev.h"

#define GIP_RHPORT 0
#define GIP_CLASS 0xFF
#define GIP_SUB   0x47
#define GIP_PROTO 0xD0

static uint8_t g_ep_in, g_ep_out;
static bool    g_mounted;
static uint8_t g_out[64] TU_ATTR_ALIGNED(4);
static uint8_t g_tx[64]  TU_ATTR_ALIGNED(4);

TU_ATTR_WEAK void gip_dev_rx(const uint8_t *data, uint16_t len) { (void) data; (void) len; }
TU_ATTR_WEAK void gip_dev_mounted(void) {}

bool gip_dev_ready(void) { return g_mounted && g_ep_in != 0; }

bool gip_dev_send(const uint8_t *data, uint16_t len) {
    if (!gip_dev_ready() || len == 0 || len > sizeof(g_tx)) return false;
    if (usbd_edpt_busy(GIP_RHPORT, g_ep_in)) return false;   // previous IN still in flight
    memcpy(g_tx, data, len);
    return usbd_edpt_xfer(GIP_RHPORT, g_ep_in, g_tx, len, false);
}

static void gd_init(void) {}
static bool gd_deinit(void) { return true; }
static void gd_reset(uint8_t rhport) { (void) rhport; g_ep_in = g_ep_out = 0; g_mounted = false; }

static uint16_t gd_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != GIP_CLASS || itf->bInterfaceSubClass != GIP_SUB ||
        itf->bInterfaceProtocol != GIP_PROTO)
        return 0;

    // Walk the whole remaining config; open interrupt endpoints (GIP data),
    // skip iso/bulk. Claim everything so all interfaces are accounted for.
    const uint8_t *p = (const uint8_t *) itf;
    uint16_t used = 0;
    while (used + 2 <= max_len) {
        uint8_t len = tu_desc_len(p);
        if (len == 0) break;
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *) p;
            if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                usbd_edpt_open(rhport, ep);
                if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) g_ep_in = ep->bEndpointAddress;
                else g_ep_out = ep->bEndpointAddress;
            }
        }
        used += len;
        p = tu_desc_next(p);
    }

    g_mounted = true;
    if (g_ep_out) usbd_edpt_xfer(rhport, g_ep_out, g_out, sizeof g_out, false); // arm OUT
    gip_dev_mounted();
    return max_len; // this driver owns the entire config
}

static bool gd_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (req->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
        switch (req->bRequest) {
            case TUSB_REQ_SET_INTERFACE: return tud_control_status(rhport, req);
            case TUSB_REQ_GET_INTERFACE: {
                static uint8_t alt = 0;
                return tud_control_xfer(rhport, req, &alt, 1);
            }
            default: break;
        }
    }
    return false; // stall anything else
}

static bool gd_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t xferred) {
    if (ep == g_ep_out) {
        if (result == XFER_RESULT_SUCCESS && xferred > 0) gip_dev_rx(g_out, (uint16_t) xferred);
        usbd_edpt_xfer(rhport, g_ep_out, g_out, sizeof g_out, false); // re-arm
    }
    return true;
}

static const usbd_class_driver_t gip_dev_driver = {
    .name = "gipdev",
    .init = gd_init,
    .deinit = gd_deinit,
    .reset = gd_reset,
    .open = gd_open,
    .control_xfer_cb = gd_control,
    .xfer_cb = gd_xfer,
    .xfer_isr = NULL,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &gip_dev_driver;
}
