# DualSense → Xbox Series passthrough adapter

Goal: let a **DualSense** be used on a **retail Xbox Series/One console** by having the
Pico 2 W act as a Gaming Input Protocol (GIP) controller to the console, while relaying
the console's **XSM3 security handshake** to a genuine Xbox Series controller connected
over USB (kept intact — no disassembly).

This is a ground-up rework, not a tweak of the existing DualSense→PC bridge. The only
reused piece is the BTStack DualSense link (`bt.cpp`). The HID + UAC1 audio + Opus/haptics
stack is **not used** on this target.

---

## Why this is the only viable design

A retail Xbox challenges the controller with XSM3 (asymmetric crypto). The signing key
lives in a licensed Microsoft security processor inside genuine controllers and has never
been publicly extracted. We do **not** attempt to break it. We relay the challenge to a
real controller and pass its signed responses back to the console. The crypto is done by
the genuine chip; we are a transport relay.

Consequence: a genuine Series controller must stay **tethered and powered** to the dongle
during use, because the console can re-issue auth at any time, not only at boot.

---

## Target topology

```
DualSense ──BT (BTStack, CYW43)──▶ Pico 2 W ──native USB (device, GIP)──▶ Xbox console
                                      │
                                      └── PIO-USB (host) ──▶ genuine Series controller (USB-C, stock)
```

Three concurrent stacks on one RP2350:
1. **BT host** — DualSense. Reuse existing `bt.cpp` link + `on_bt_data` report parsing.
2. **Native USB device** — presents to the console as a GIP controller (vendor class, not HID).
3. **PIO-USB host** — enumerates the donor Series controller; used only for the auth relay
   (and optionally to mirror its idle/keepalive traffic).

### Silicon constraints / risks (validate before committing)
- RP2350 has **one native USB block**; it's the device side. The donor host **must** run on
  **Pico-PIO-USB** (Full-Speed software USB). GIP controllers are Full-Speed → compatible.
- **PIO/DMA contention:** CYW43 (Wi-Fi/BT) uses a PIO state machine; PIO-USB needs its own
  PIO + DMA + a spare core for its interrupt-timed loop. RP2350 has 3 PIO blocks — likely
  enough, but must be proven. PIO-USB effectively wants a dedicated core (core1); BTStack +
  GIP device + main loop live on core0.
- TinyUSB has **no GIP host class driver**. The donor is talked to via the **vendor/raw
  endpoint** host path (custom transfers), not a stock class driver.
- **Timing:** the console enforces auth-response deadlines. Relay latency across
  PIO-USB → core boundary must stay inside the console's timeout window.

---

## The unknown we must capture empirically

GIP **transport framing** is documented (Linux `xone` driver, `medusalix/xone`). What is
*not* documented is the exact **XSM3 auth packet sequence** the console emits and the timing
it expects. We cannot hardcode it blind. Plan requires a capture step:

- Sniff the console↔genuine-controller USB traffic during a real boot/auth (USB protocol
  analyzer, or a passthrough logging build on the Pico once dual-USB works).
- Identify the auth vendor packets vs. normal input/keepalive.
- Relay only what must be relayed; synthesize the rest.

If Phase 0/1 shows PIO-USB host + native device + CYW43 can't coexist with adequate timing,
the whole approach is blocked — treat Phase 0 as a go/no-go gate.

---

## Phased plan (each phase gates the next)

### Phase 0 — Feasibility gate  ✅ PIO-USB host proven on hardware
- Bring up **Pico-PIO-USB host** on GP2/GP3 (D+ = GP2). DONE: enumerates a USB device on
  the host port (validated with a USB-serial / T1S dongle), OLED status on I2C1 GP10/GP11,
  host stack on core1. Targets `usbhost_test`, `gip_probe`.
- Remaining before Phase 1: capture the **genuine Series controller's** descriptors +
  GIP input bytes with `gip_probe` (needs the controller). Full dual-role (native device +
  PIO host + CYW43/BT together) still to be shaken out — SDK blocks stdio-USB while a host
  is linked, so serial logging wants a UART adapter or an SWD probe.

### Phase 1 — Present as a GIP device to the console
- Build GIP USB descriptors (vendor class, correct VID/PID/interfaces/endpoints) so the
  Xbox begins enumeration + auth. Sourced from `xone` + captured descriptors of the donor.
- **Exit criteria:** console starts the auth exchange (even if it then times out).

### Phase 2 — Auth relay (the crux)
- Capture the console's auth packets; forward them over PIO-USB to the donor; return the
  donor's signed responses to the console within the timeout.
- **Exit criteria:** console accepts the device as an authenticated controller.

### Phase 3 — Input mapping DualSense → GIP
- Map the DualSense report (parsed today in `on_bt_data`, report `0x31`) into GIP input
  reports: sticks, triggers (analog), face/shoulder/dpad, share/menu/view/xbox (guide).
- Rumble: map GIP force-feedback (dual-motor) back onto DualSense output reports (basic
  rumble only; HD haptics not represented).
- **Exit criteria:** DualSense drives a game on the console.

### Phase 4 — Robustness
- Handle re-auth mid-session, donor unplug, DualSense disconnect/reconnect, watchdog, LED
  status. Guide button behavior.

---

## What is reused vs. new

| Area | Status |
|---|---|
| `bt.cpp` DualSense BTStack link | **Reused** (input parsing, output/rumble writes) |
| `btstack_config.h` | Reused, single connection |
| `usb_descriptors.c` (DualSense HID+UAC1) | **Replaced** with GIP vendor descriptors |
| `usb.cpp` (UAC1 audio ctrl) | **Dropped** on this target |
| `audio.cpp`, Opus, WDL resampler | **Dropped** (no HD haptics on Xbox) |
| `main.cpp` loop | **Reworked**: core1 = PIO-USB host; core0 = GIP device + BT + relay |
| PIO-USB host + GIP relay | **New** |

## Key external references
- `medusalix/xone` — Linux Xbox One/Series driver; GIP transport framing.
- `sekigon-gonnoc/Pico-PIO-USB` — software USB host on RP2xxx PIO.
- TinyUSB dual-role (device + PIO-USB host) examples.
- Existing DualSense report structure docs (already cited in README).

## Status (updated)
- **Phase 0** ✅ PIO-USB host enumerates devices.
- **Phase 1** ✅ Read the genuine controller over GIP via a custom host class driver
  (`src/gipdrv/gip_host.c`) — input (0x20) + status (0x03) stream. Raw `tuh_edpt` does NOT
  work (endpoints aren't polled unless a class driver claims the interface).
- **Concurrency** ✅ device (native USB) + host (PIO-USB) run together, stable.
- **Transparent relay** ✅ `src/passthru` — console↔Pico↔controller, every GIP packet relayed
  (incl. auth 0x06). **Verified on Windows: the relayed controller navigates Steam Big
  Picture** — full pipeline (enumerate + GIP handshake relay + input) works on a real host.
- **v2 input substitution**: `src/passthru/ds_to_gip.h` maps a DualSense report → GIP input;
  wired into the relay behind `g_ds_valid` (off until BT is integrated).

### Remaining
1. **Real Xbox test** of the relay — the console enforces XSM3 strictly; the relay forwards
   the genuine controller's auth, so it should pass, but only the console confirms it.
2. **DualSense BT integration** (activate v2): bring up the BT link (reuse `bt.cpp` +
   `btstack_config.h`), parse the DualSense report into `g_ds`, set `g_ds_valid`. Adds a
   third concurrent stack (CYW43/BT) — verify coexistence at 120 MHz.
3. Robustness: re-auth, disconnect/reconnect, guide button, status UI.

### Planned features (not dropped — deferred)
- **Audio relay (headset).** The Xbox controller exposes an audio interface (IF1, iso
  endpoints 0x83 IN / 0x03 OUT) — chat audio runs through the controller. To support a
  headset we must relay that audio path between console and the donor controller (iso
  endpoints on both device+host sides), and bridge the DualSense's own audio (BT headset /
  the DualSense 3.5mm/speaker). Non-trivial (iso endpoints + resampling) — a dedicated phase,
  but part of the goal, NOT permanently disabled.
- **Adaptive triggers <- impulse triggers.** The Xbox controller has impulse (trigger)
  rumble carried in the GIP rumble command (0x09) with left/right trigger motor levels. Map
  those onto the DualSense **adaptive trigger** effects (resistance/vibration) so the console's
  trigger-haptic intent is felt on the DualSense. Also map the main dual-motor rumble to the
  DualSense actuators. Requires sending DualSense output reports over the existing BT link
  (bt_write) — the plumbing already exists in bt.cpp.

### Build targets
`bringup_test` (LED+OLED), `usbhost_test`, `gip_probe`, `gip_capture`, `class_test`,
`gip_hostdrv` (read controller), `xbox_dev` (device spoof), **`passthrough`** (the product).
Debug: FTDI on GP0/GP1 @115200; serial byte 'b' → BOOTSEL for button-free reflash.
