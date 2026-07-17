//
// TinyUSB dual-role config for the passthrough:
//   DEVICE (rhport 0, native USB)  -> Xbox console  (GIP vendor class)
//   HOST   (rhport 1, PIO-USB)     -> genuine controller (our GIP class driver)
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

//------------- DEVICE (rhport 0) -> console -------------//
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT        0
#endif
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_VENDOR          1
#define CFG_TUD_VENDOR_RX_BUFSIZE  256
#define CFG_TUD_VENDOR_TX_BUFSIZE  256
#define CFG_TUD_CDC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MSC             0
#define CFG_TUD_AUDIO           0

//------------- HOST (rhport 1, PIO-USB) -> controller -------------//
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1
#define BOARD_TUH_RHPORT        1
#define CFG_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB             0
#define CFG_TUH_DEVICE_MAX      1
#define CFG_TUH_HID             0
#define CFG_TUH_MSC             0
#define CFG_TUH_CDC             0
#define CFG_TUH_VENDOR          0

#ifdef __cplusplus
}
#endif
#endif
