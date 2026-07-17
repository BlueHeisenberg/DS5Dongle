//
// GIP host class driver — registered via usbh_app_driver_get_cb().
//
#include "tusb.h"
#include "host/usbh_pvt.h"
#include "gip_host.h"

#define GIP_CLASS 0xFF
#define GIP_SUB   0x47
#define GIP_PROTO 0xD0

static struct {
    uint8_t  daddr;
    uint8_t  itf_num;
    uint8_t  ep_in, ep_out;
    uint16_t epin_size;
    bool     active;
} gip;

static uint8_t g_epin[64];
static uint8_t g_epout[64];
static uint8_t g_seq = 1;

// Weak defaults so the app can override.
TU_ATTR_WEAK void gip_host_rx(const uint8_t *data, uint16_t len) { (void) data; (void) len; }
TU_ATTR_WEAK void gip_host_mounted(uint8_t daddr) { (void) daddr; }

bool gip_host_send(uint8_t cmd, uint8_t opt, const uint8_t *payload, uint8_t len) {
    if (!gip.active || len > 60) return false;
    g_epout[0] = cmd; g_epout[1] = opt; g_epout[2] = g_seq++; g_epout[3] = len;
    if (len) memcpy(g_epout + 4, payload, len);
    return usbh_edpt_xfer(gip.daddr, gip.ep_out, g_epout, (uint16_t) (4 + len));
}

static bool gip_init(void) { tu_memclr(&gip, sizeof gip); return true; }

static uint16_t gip_open(uint8_t rhport, uint8_t daddr,
                         const tusb_desc_interface_t *itf, uint16_t max_len) {
    (void) rhport;
    if (itf->bInterfaceClass != GIP_CLASS || itf->bInterfaceSubClass != GIP_SUB ||
        itf->bInterfaceProtocol != GIP_PROTO)
        return 0;

    const uint8_t *p = (const uint8_t *) itf;
    uint16_t drv_len = tu_desc_len(p);       // interface descriptor
    p = tu_desc_next(p);

    uint8_t eps = 0, in = 0, out = 0;
    uint16_t insz = 0;
    bool has_interrupt = false;

    while (eps < itf->bNumEndpoints && drv_len < max_len) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *) p;
            if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                has_interrupt = true;
                if (!gip.active) {
                    tuh_edpt_open(daddr, ep);
                    if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                        in = ep->bEndpointAddress; insz = tu_edpt_packet_size(ep);
                    } else {
                        out = ep->bEndpointAddress;
                    }
                }
            }
            eps++;
        }
        drv_len += tu_desc_len(p);
        p = tu_desc_next(p);
    }

    // Latch only the interrupt (GIP data) interface as ours.
    if (has_interrupt && !gip.active) {
        gip.daddr = daddr; gip.itf_num = itf->bInterfaceNumber;
        gip.ep_in = in; gip.ep_out = out; gip.epin_size = insz ? insz : 64;
        gip.active = true;
    }
    return drv_len; // consume this interface's descriptors regardless
}

static bool gip_set_config(uint8_t daddr, uint8_t itf_num) {
    if (gip.active && gip.daddr == daddr && gip.itf_num == itf_num) {
        usbh_edpt_xfer(daddr, gip.ep_in, g_epin, gip.epin_size); // start polling IN
        uint8_t on = 0x00;
        gip_host_send(0x05, 0x20, &on, 1);                        // GIP power on
        gip_host_mounted(daddr);
    }
    usbh_driver_set_config_complete(daddr, itf_num);
    return true;
}

static bool gip_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred) {
    if (ep_addr == gip.ep_in) {
        if (result == XFER_RESULT_SUCCESS && xferred > 0) gip_host_rx(g_epin, (uint16_t) xferred);
        usbh_edpt_xfer(daddr, gip.ep_in, g_epin, gip.epin_size); // re-arm
    }
    return true;
}

static void gip_close(uint8_t daddr) {
    if (gip.daddr == daddr) tu_memclr(&gip, sizeof gip);
}

static const usbh_class_driver_t gip_driver = {
    .name       = "gip",
    .init       = gip_init,
    .deinit     = NULL,
    .open       = gip_open,
    .set_config = gip_set_config,
    .xfer_cb    = gip_xfer_cb,
    .close      = gip_close,
};

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &gip_driver;
}
