//
// TinyUSB host config for the class-driver test (MSC pendrive + HID).
// Uses TinyUSB's own class drivers to prove PIO-USB IN transfers work.
//
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_
#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_DEBUG          0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1
#define BOARD_TUH_RHPORT        1
#define CFG_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HUB             1
#define CFG_TUH_DEVICE_MAX      (3 * CFG_TUH_HUB + 1)

#define CFG_TUH_MSC             1
#define CFG_TUH_HID             4
#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64
#define CFG_TUH_CDC             0
#define CFG_TUH_VENDOR          0

#ifdef __cplusplus
}
#endif
#endif
