# Project STATUS & continuity (read this first)

DualSense → Xbox passthrough on a Pico 2 W. This file is the single source of truth
for *where we are* and *what to do next*. Companion docs: `GIP_NOTES.md` (protocol +
captured raw data), `XBOX_PASSTHROUGH.md` (architecture/plan), `BUILD_AND_TEST.md`.

## Goal
A Pico 2 W lets a **DualSense** be used on a **real Xbox Series console**:
- DualSense connects over **Bluetooth** (reuse `src/bt.cpp`).
- Pico presents to the console as a **GIP Xbox controller** over native USB (device side).
- A **genuine Xbox Series controller** (VID 045E, PID 0B12) is wired to the Pico's
  **PIO-USB host** port (GP2/GP3) and answers the console's **XSM3 auth** (relayed live —
  the crypto can't be recorded/cloned; the chip must respond).
- DualSense input is injected as GIP input; controller's own input is dropped.

## Hardware / wiring (all verified)
- Pico 2 W. Native USB-C → host (PC for dev / Xbox for real).
- **PIO-USB host** (donor controller): D+ = GP2, D- = GP3, VBUS = pin 40, GND. PIO-USB on **PIO1**.
- **OLED SSD1306 128x32**: SDA = GP10, SCK = GP11, VCC = 3V3 (pin 36), GND = pin 38 (I2C1).
- **FTDI CP2102 serial**: RXD ← GP0, TXD → GP1 (3.3V!), GND. **COM12 @ 115200**. This is our
  live debug channel and it stays on the PC even when the Pico's native USB is on the Xbox.
- HDMI capture card exists ("USB3. 0 capture", MS2109) but **does not work** — ignore it.

## What WORKS (verified on hardware)
- ✅ PIO-USB host reads the genuine controller (needs a custom host class driver
  `src/gipdrv/gip_host.c` — raw `tuh_edpt` does NOT get polled).
- ✅ 4-stack concurrency: native USB device + PIO-USB host + CYW43/BT + relay, all at 120 MHz.
  Fix that made it work: **PIO-USB on PIO1** + **init PIO-USB (core1) BEFORE cyw43_arch_init**.
- ✅ DualSense over BT streams **~770 Hz (1 ms)**. Both USB hops forced to **1 ms** poll.
- ✅ Input **decoupled** from the controller carrier: we generate our own GIP 0x20 from the
  DualSense (controller often sends CTRL 0Hz on a lenient host, so carrier-piggyback fails).
- ✅ Full passthrough works on a **PC host** (Steam Big Picture navigated by DualSense).
- ✅ Custom **device** class driver `src/xbox/gip_dev.c` presents the real controller's full
  3-interface descriptor and **configures on a PC** (console=1). Stock `tud_vendor` could NOT
  (it can't present iso/bulk/alt endpoints and needs every interface claimed).

## THE CURRENT BLOCKER (where we are right now)
On a **real Xbox**, the device is **never configured** (`HOST --`, `console=0`). The Xbox
**reads our descriptors then rejects** — telemetry counter shows `descReq dev=2 cfg=1` then stop.
Ruled out: cable (2 tried), port (front+back), power (controller unplugged = same),
interface topology (single-iface and full-3-iface give the *identical* dev=2 cfg=1 pattern),
`bcdDevice` (now exact 0x0509), BOS (real controller has none — confirmed STALL).

**Leading hypothesis: Microsoft OS descriptor.** Real Xbox controllers answer a
**string index 0xEE** request (MS OS 1.0 string → "MSFT100" + a vendor request code), then a
**vendor control request** returning a GIP compatible-ID descriptor. Windows is lenient; the
Xbox likely *requires* it. We have NOT captured string 0xEE. The `dev=2 cfg=1 then stop`
pattern fits: Xbox reads device+config header, requests 0xEE, we STALL (only strings 0-3),
Xbox aborts before SET_CONFIGURATION.

## IMMEDIATE NEXT STEP (resume here)
1. Extend `src/gipprobe/gip_probe.cpp` to also request **string index 0xEE** (langid 0) and,
   if it's an MS OS string, do the follow-up **vendor control transfer** (bmRequestType 0xC0,
   bRequest = the vendor code byte from the 0xEE string, wIndex = 0x0004 compat ID) and dump it.
   (Was mid-edit: str_cb should, after string 3, fetch 0xEE; then done.)
   ALSO add the `b`→BOOTSEL serial trigger to gip_probe (it's the only firmware still missing it).
2. Run it with the controller on the host port; read COM12 → get string 0xEE + MS OS descriptor.
3. Implement on the device side: respond to string 0xEE and the MS OS vendor request in
   `src/xbox/gip_dev.c` / `xbox_descriptors.c` (add `tud_vendor_control_xfer_cb`-style handling;
   our custom driver's `gd_control` currently stalls unknown requests — needs to answer the
   MS OS vendor request).
4. Flash, move Pico USB to Xbox, replug controller, read COM12 for `console=1`.
5. If accepted → watch `c2h`/`h2c` for the auth relay; then verify DualSense drives the console.

If string 0xEE also STALLs on the real controller (no MS OS descriptor), pivot: enable
`CFG_TUSB_DEBUG=2` on the device to log the exact enumeration step where the Xbox aborts.

## Firmware targets (all build; `ninja -C build <target>`)
- `bringup_test` — LED + OLED smoke test.
- `usbhost_test` — PIO-USB host enumerates a device.
- `gip_probe` — dump a device's descriptors over serial (device+config+BOS+strings).
- `gip_capture` — GIP host handshake capture to flash.
- `class_test` — MSC/HID class-driver probe (proved PIO-USB IN works via class driver).
- `gip_hostdrv` — read controller via custom host class driver.
- `xbox_dev` — early device spoof (single interface).
- **`passthrough`** — THE PRODUCT: BT DualSense + PIO-USB host + custom device driver + relay.

## Flash / debug workflow
- Flashing needs the Pico's **native USB on the PC** in BOOTSEL. `picotool load -x build/<t>.uf2`.
- **Button-free BOOTSEL:** send byte `b` to COM12 → firmware calls `reset_usb_boot()`.
  ALL firmwares must include this (getchar_timeout_us in the main loop). gip_probe still lacks it — ADD IT.
- After each reflash, the donor controller must be **unplugged/replugged** (loses its host session).
- Read telemetry: open COM12 @115200; `passthrough` prints `[pt] console=.. ctrl=.. ds=.. |
  DS ..Hz CTRL ..Hz | descReq dev=.. cfg=..` every second.
- Build env (Bash):
  ```
  export PICO_SDK_PATH=/c/Users/ti_ra/.pico-sdk/sdk/2.2.0
  export PICO_TOOLCHAIN_PATH=/c/Users/ti_ra/.pico-sdk/toolchain/14_2_Rel1
  export PATH=/c/Users/ti_ra/.pico-sdk/cmake/v3.30.5/bin:/c/Users/ti_ra/.pico-sdk/ninja/v1.12.1:/c/Users/ti_ra/AppData/Local/Programs/Python/Python312:$PATH
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w \
    -Dpicotool_DIR=C:/Users/ti_ra/.pico-sdk/picotool/2.2.0-a4/picotool \
    -Dpioasm_DIR=C:/Users/ti_ra/.pico-sdk/sdk-tools/2.2.0/pioasm
  ```
  picotool: `/c/Users/ti_ra/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe`

## Git / environment
- Remote **`bh`** = `https://github.com/BlueHeisenberg/DS5Dongle.git` (fork of awalol/DS5Dongle
  = `origin`). Branch **`feature/xbox-passthrough`** tracks `bh`.
- `gh` active account = **BlueHeisenberg** (`gh auth switch --user davidperezstark` to revert).
- Commits authored as **BlueHeisenberg** (`2033896+BlueHeisenberg@users.noreply.github.com`),
  set via repo-local git config. Commit trailers: Co-Authored-By Claude + Claude-Session.
- Portable ffmpeg (for the dead capture card) is in the scratchpad; not needed.

## Deferred features (planned, user wants them — NOT dropped)
- **Headset audio relay** (controller IF1 iso ↔ DualSense audio). Big iso-endpoint phase.
- **Adaptive-trigger / rumble** already implemented (GIP 0x09 → DualSense output via bt_write);
  needs a game sending rumble to fine-tune the trigger-vibration effect offsets.
