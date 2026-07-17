//
// Custom TinyUSB DEVICE class driver for the Xbox controller spoof.
// Presents the real controller's full multi-interface GIP topology (which the
// stock vendor class can't) so the Xbox accepts and configures the device.
//
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gip_dev_ready(void);                              // configured + IN endpoint open
bool gip_dev_send(const uint8_t *data, uint16_t len);  // send a GIP packet to the console

// App-implemented hooks (weakly defaulted in the driver).
void gip_dev_rx(const uint8_t *data, uint16_t len);    // console -> us (OUT endpoint)
void gip_dev_mounted(void);                            // configured by the host

#ifdef __cplusplus
}
#endif
