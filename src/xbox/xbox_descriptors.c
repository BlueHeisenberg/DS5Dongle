//
// USB descriptors for the Xbox One/Series controller DEVICE spoof.
// Signature values (VID 0x045E, class/sub/proto 0xFF/0x47/0xD0, two 64B interrupt
// endpoints) are the documented wired Xbox One controller identity. The GIP-level
// "announce" identity is NOT here — that gets captured from the real pad.
//
#include "tusb.h"

#define EP_GIP_IN   0x81
#define EP_GIP_OUT  0x01
#define EP_SIZE     64

//--------------------------------------------------------------------+
// Device descriptor
//--------------------------------------------------------------------+
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0xFF,   // vendor specific
    .bDeviceSubClass    = 0x47,   // GIP
    .bDeviceProtocol    = 0xD0,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x045E, // Microsoft
    .idProduct          = 0x0B12, // Xbox Series X|S Controller (matches donor)
    .bcdDevice          = 0x0509, // exact value captured from the donor
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// Diagnostics: how many times the host asks for our descriptors. If these climb
// but the device never configures, the host is reading and REJECTING us (descriptor
// content). If they stay 0, the host isn't talking to us at all (physical/cable).
volatile uint32_t g_dev_desc_reqs = 0;
volatile uint32_t g_cfg_desc_reqs = 0;

const uint8_t *tud_descriptor_device_cb(void) {
    g_dev_desc_reqs++;
    return (const uint8_t *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration descriptor: one vendor interface, 2 interrupt endpoints
//--------------------------------------------------------------------+
// EXACT clone of the real Xbox Series controller (045E:0B12) configuration
// descriptor, captured from the donor via gip_capture. 3 interfaces:
//   IF0 = GIP data (interrupt EP 0x82 IN / 0x02 OUT)   <- the one we drive
//   IF1 = audio    (isochronous, alt1)                  <- declared, not serviced yet
//   IF2 = bulk data                                     <- declared, not serviced yet
// The Xbox validates this topology; a single-interface device gets rejected.
// EXACT clone of the real Xbox Series controller config descriptor (captured
// from the donor). The custom device class driver (gip_dev.c) claims all three
// interfaces so SET_CONFIGURATION succeeds; only IF0's interrupt EPs are opened.
static const uint8_t desc_configuration[] = {
    0x09, 0x02, 0x77, 0x00, 0x03, 0x01, 0x00, 0xA0, 0xFA, // config: wTotalLen=0x77, 3 ifaces, 500mA
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x47, 0xD0, 0x00, // IF0 alt0 (GIP data)
    0x07, 0x05, 0x02, 0x03, 0x40, 0x00, 0x04,             //   EP 0x02 OUT interrupt 64
    0x07, 0x05, 0x82, 0x03, 0x40, 0x00, 0x04,             //   EP 0x82 IN  interrupt 64
    0x09, 0x04, 0x00, 0x01, 0x02, 0xFF, 0x47, 0xD0, 0x00, // IF0 alt1
    0x07, 0x05, 0x02, 0x03, 0x40, 0x00, 0x04,
    0x07, 0x05, 0x82, 0x03, 0x40, 0x00, 0x02,
    0x09, 0x04, 0x01, 0x00, 0x00, 0xFF, 0x47, 0xD0, 0x00, // IF1 alt0 (audio, no EPs)
    0x09, 0x04, 0x01, 0x01, 0x02, 0xFF, 0x47, 0xD0, 0x00, // IF1 alt1 (audio iso)
    0x07, 0x05, 0x03, 0x01, 0xE4, 0x00, 0x01,
    0x07, 0x05, 0x83, 0x01, 0x40, 0x00, 0x01,
    0x09, 0x04, 0x02, 0x00, 0x00, 0xFF, 0x47, 0xD0, 0x00, // IF2 alt0 (bulk, no EPs)
    0x09, 0x04, 0x02, 0x01, 0x02, 0xFF, 0x47, 0xD0, 0x00, // IF2 alt1 (bulk)
    0x07, 0x05, 0x04, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x84, 0x02, 0x40, 0x00, 0x00,
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    g_cfg_desc_reqs++;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String descriptors
//--------------------------------------------------------------------+
static const char *string_desc[] = {
    (const char[]){0x09, 0x04}, // 0: English (US)
    "Microsoft",                // 1: Manufacturer
    "Controller",               // 2: Product
    "000000000001",             // 3: Serial
};

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    uint8_t chr_count;

    if (index == 0) {
        desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0])) return NULL;
        const char *str = string_desc[index];
        chr_count = (uint8_t) strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) desc_str[1 + i] = str[i];
    }

    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
