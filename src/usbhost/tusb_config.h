//
// TinyUSB config for the PIO-USB HOST bring-up test.
// Host stack only, on rhport 1 (PIO-USB). No class drivers — we only need
// enumeration + device descriptors to prove the host port works.
//
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// CFG_TUSB_OS is provided by the Pico SDK build (OPT_OS_PICO) — don't redefine.
#define CFG_TUSB_DEBUG          0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

//------------- HOST on rhport 1 (PIO-USB) -------------//
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1
#define BOARD_TUH_RHPORT        1
#define CFG_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Allow a hub + a few devices (some dongles present as composite/hub).
#define CFG_TUH_HUB             1
#define CFG_TUH_DEVICE_MAX      (3 * CFG_TUH_HUB + 1)

// No class drivers needed for an enumeration test.
#define CFG_TUH_HID             0
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_VENDOR          0

#ifdef __cplusplus
}
#endif

#endif // TUSB_CONFIG_H_
