# Build, flash & test — Xbox passthrough

## Hardware
- Raspberry Pi **Pico 2 W** (RP2350).
- **Donor Xbox Series controller** on the PIO-USB host port: USB-A breakout with
  **D+ → GP2, D- → GP3, VBUS → pin 40, GND → GND** (22–27 Ω series on D+/D- recommended),
  connected to the controller with a USB-C-to-USB-A cable.
- **I²C OLED** (SSD1306 128×32): **SDA → GP10, SCK → GP11, VCC → 3V3 (pin 36), GND → pin 38**.
- **FTDI/CP2102 USB-serial** for logs + button-free flashing: **RXD → GP0 (pin 1),
  GND → pin 8**, optional **TXD → GP1 (pin 2)** at 3.3 V. 115200 baud.
- The Pico's own USB-C goes to the **host** (PC for dev, Xbox for the real thing).

## Build
```bash
export PICO_SDK_PATH="/c/Users/ti_ra/.pico-sdk/sdk/2.2.0"
export PICO_TOOLCHAIN_PATH="/c/Users/ti_ra/.pico-sdk/toolchain/14_2_Rel1"
export PATH="/c/Users/ti_ra/.pico-sdk/cmake/v3.30.5/bin:/c/Users/ti_ra/.pico-sdk/ninja/v1.12.1:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w \
  -Dpicotool_DIR=C:/Users/ti_ra/.pico-sdk/picotool/2.2.0-a4/picotool \
  -Dpioasm_DIR=C:/Users/ti_ra/.pico-sdk/sdk-tools/2.2.0/pioasm
ninja -C build passthrough        # or any target below
```

## Flashing (button-free)
Every firmware here watches the serial line for the byte **`b`** and calls
`reset_usb_boot()`. So after the first BOOTSEL-button flash:
```bash
# drop to BOOTSEL over serial (COM port = the FTDI):
#   send 'b' to the COM port @115200, then:
picotool load -x build/passthrough.uf2
```
First time only: hold BOOTSEL while plugging USB, then `picotool load -x ...`.

## Targets (build order they were developed in)
| Target | Purpose |
|---|---|
| `bringup_test` | LED + I²C OLED smoke test |
| `usbhost_test` | PIO-USB host enumerates a device (VID/PID) |
| `gip_probe` | dump a device's USB descriptors |
| `gip_capture` | drive controller as GIP host, capture packets to flash |
| `class_test` | MSC/HID class-driver probe (proved PIO-USB IN works) |
| `gip_hostdrv` | read the controller via the custom GIP class driver |
| `xbox_dev` | spoof an Xbox controller to a host (device side) |
| **`passthrough`** | **the product**: DualSense→Xbox relay + input substitution |

## Testing `passthrough`
Read status on the OLED (or serial `[pt] hb ...`):
`XB±` host detected · `CT±` donor controller mounted · `DS±` DualSense linked ·
`->`/`<-` relay packet counts · `DS bat`/stick values.

1. **On PC:** plug Pico USB to PC, controller on host port, pair the DualSense
   (PS + Create until light-bar double-flash). Expect `XB+ CT+ DS+`, both relay
   counters climbing, and the **DualSense** driving an Xbox controller in Steam Big
   Picture / gamepad-tester.
2. **On Xbox:** move the Pico USB to the console. Watch for the console to accept the
   relayed controller (auth). This is the final validation.

## Known-good result
Verified on a PC host: all four subsystems run together (device + PIO-USB host + BT +
relay); the DualSense drives input while the donor controller answers auth.

## Deferred (planned, not disabled)
- Headset **audio relay** (controller IF1 iso endpoints ↔ DualSense audio).
- **Adaptive-trigger / rumble** mapping (GIP rumble 0x09 → DualSense output report).
See `docs/XBOX_PASSTHROUGH.md`.
