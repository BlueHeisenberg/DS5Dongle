//
// TinyUSB config for the Xbox-controller DEVICE spoof (native USB, rhport 0).
// Milestone 1: enumerate as an Xbox controller and capture the host's GIP init.
//
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// CFG_TUSB_OS is provided by the Pico SDK build.
#define CFG_TUSB_DEBUG          0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

//------------- DEVICE on rhport 0 (native USB) -------------//
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT        0
#endif
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

// One vendor interface carrying the GIP data endpoints.
#define CFG_TUD_VENDOR          1
#define CFG_TUD_VENDOR_RX_BUFSIZE  256
#define CFG_TUD_VENDOR_TX_BUFSIZE  256

#define CFG_TUD_CDC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MSC             0
#define CFG_TUD_AUDIO           0

#ifdef __cplusplus
}
#endif

#endif // TUSB_CONFIG_H_
