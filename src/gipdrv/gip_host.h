//
// Minimal GIP host class driver (TinyUSB app driver). Claims the Xbox
// controller's vendor interface (class FF/47/D0) so its interrupt endpoints are
// properly opened and polled by the host stack.
//
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Send a GIP packet to the controller (header cmd/opt/seq/len + payload).
bool gip_host_send(uint8_t cmd, uint8_t opt, const uint8_t *payload, uint8_t len);

// App-implemented hooks (weakly defaulted in the driver).
void gip_host_rx(const uint8_t *data, uint16_t len);  // a GIP packet arrived from the controller
void gip_host_mounted(uint8_t daddr);                 // GIP interface claimed + powered on

#ifdef __cplusplus
}
#endif
