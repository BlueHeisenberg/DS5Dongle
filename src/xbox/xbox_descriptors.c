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
    .bcdDevice          = 0x0407,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration descriptor: one vendor interface, 2 interrupt endpoints
//--------------------------------------------------------------------+
#define CONFIG_TOTAL_LEN  (9 + 9 + 7 + 7)

static const uint8_t desc_configuration[] = {
    // Configuration: 1 interface, bus-powered, 500mA
    9, TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(CONFIG_TOTAL_LEN), 1, 1, 0, 0x80, 250,

    // Interface 0: vendor 0xFF / 0x47 / 0xD0, 2 endpoints
    9, TUSB_DESC_INTERFACE, 0, 0, 2, 0xFF, 0x47, 0xD0, 0,

    // Endpoint IN 0x82, interrupt, 64B, interval 1 (1ms poll -> lower latency)
    7, TUSB_DESC_ENDPOINT, EP_GIP_IN, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(EP_SIZE), 1,

    // Endpoint OUT 0x02, interrupt, 64B, interval 1
    7, TUSB_DESC_ENDPOINT, EP_GIP_OUT, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(EP_SIZE), 1,
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
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
