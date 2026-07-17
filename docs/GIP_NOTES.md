# GIP (Gaming Input Protocol) — working notes

Reverse-engineering reference for the Xbox passthrough, distilled from the Linux
`xone` driver (`medusalix/xone`, `bus/protocol.c` + `bus/bus.h`). This is the wire
protocol our firmware must speak on **both** sides: as a GIP *host* to the genuine
Series controller (over PIO-USB), and as a GIP *device* to the console (native USB).

> Auth note: we never decode XSM3. We identify auth packets and **relay them verbatim**
> between console and the genuine controller. The controller's security chip does the crypto.

## Packet framing

```
Byte 0 : command
Byte 1 : options  (see bits below; low nibble = client id, GIP_HDR_CLIENT_ID = bits 3:0)
Byte 2 : sequence number
Byte 3+: packet length  (variable-length int)
[opt]  : chunk offset    (variable-length int, only if chunked)
```
- `GIP_HDR_MIN_LENGTH = 3`; header padded to even length (`len + (len % 2)`).

### Option/flag bits (byte 1)
```
GIP_OPT_ACKNOWLEDGE = BIT(4)   // sender wants an ACK
GIP_OPT_INTERNAL    = BIT(5)   // "system" command (power/announce/identify/auth/led/...)
GIP_OPT_CHUNK_START = BIT(6)
GIP_OPT_CHUNK       = BIT(7)
```

## Command IDs
Internal (system, sent with `GIP_OPT_INTERNAL`):
```
GIP_CMD_ACKNOWLEDGE  = 0x01
GIP_CMD_ANNOUNCE     = 0x02
GIP_CMD_STATUS       = 0x03
GIP_CMD_IDENTIFY     = 0x04
GIP_CMD_POWER        = 0x05
GIP_CMD_AUTHENTICATE = 0x06   // <-- the XSM3 challenge/response we relay
GIP_CMD_VIRTUAL_KEY  = 0x07   // guide (Xbox) button, its own packet
GIP_CMD_AUDIO_CONTROL= 0x08
GIP_CMD_LED          = 0x0a
GIP_CMD_HID_REPORT   = 0x0b
```
Client (gameplay):
```
GIP_CMD_RUMBLE       = 0x09   // host -> controller, no GIP_OPT_INTERNAL
GIP_CMD_INPUT        = 0x20   // controller -> host, gamepad state
```

## Host bring-up sequence (what the Xbox — and our host side — sends)
1. **POWER on** — `GIP_CMD_POWER`, options `id | INTERNAL`, payload 1 byte `mode` (power-on value).
2. Controller sends **ANNOUNCE** (`0x02`) with hw/fw info → we ACK.
3. Host requests **IDENTIFY** (`0x04`, header only) → controller replies with capabilities.
4. **AUTHENTICATE** (`0x06`) challenge/response — repeated until satisfied.
5. Host sets **LED** (`0x0a`, payload `[mode][brightness]`).
6. Controller streams **INPUT** (`0x20`); periodic **STATUS** (`0x03`).

ACK packet (`gip_acknowledge_pkt`): echoes `command`, `options = id | INTERNAL`,
`length` (LE16 received so far), `remaining` (LE16 left) — used for chunked transfers.

## Standard gamepad INPUT report (cmd 0x20 payload, after the header)
Little-endian. To be **confirmed byte-exact by `gip_probe`** against the real controller:
```
+0  buttons  (u16 LE) : b0 sync, b2 menu, b3 view, b4 A, b5 B, b6 X, b7 Y,
                        b8 dpad-up, b9 down, b10 left, b11 right,
                        b12 LB, b13 RB, b14 LS, b15 RS
+2  trigger_left  (u16 LE, 0..1023)
+4  trigger_right (u16 LE, 0..1023)
+6  stick_left_x  (s16 LE)
+8  stick_left_y  (s16 LE)
+10 stick_right_x (s16 LE)
+12 stick_right_y (s16 LE)
```
Guide/Xbox button arrives separately as `GIP_CMD_VIRTUAL_KEY` (0x07), not in this report.

## Descriptors to clone (device side)
Genuine Xbox One/Series controller presents (values to be captured by `gip_probe`):
- Device: `idVendor = 0x045E` (Microsoft), `idProduct` = controller-specific.
- A **vendor-specific interface** (`bInterfaceClass = 0xFF`, sub `0x47`, proto `0xD0`)
  carrying the GIP data endpoints (one interrupt/bulk IN, one OUT).
- Additional audio interfaces exist on the real pad; the GIP data interface is the one
  the console authenticates and streams input over.

## Auth-relay strategy (the crux)
- On the **device side** (to console): when a packet with `command == 0x06` arrives on our
  OUT endpoint, forward its payload verbatim to the controller's OUT endpoint (host side).
- On the **host side** (from controller): when the controller emits `command == 0x06`,
  forward it verbatim to the console via our IN endpoint.
- Everything else (power/announce/identify/led) we can either relay or synthesize.
- Timing: the console enforces a response window. Relay latency across the core1(host)↔
  core0(device) boundary must stay inside it — measure once we have real packets.
```
console ──0x06 challenge──▶ Pico(device) ──relay──▶ controller(chip)
console ◀──0x06 response──── Pico(device) ◀──relay── controller
```

## Captured: Windows host handshake vs our device spoof (milestone 2)
Our `xbox_dev` enumerated on Windows (VID 045E recognized as an Xbox controller) and
we sent a spec-shaped ANNOUNCE (0x02). Windows replied:
```
04/4  IDENTIFY request (header only)   ×4 retries
05/5  POWER (1-byte payload)
```
→ Windows accepts announce, then demands an **IDENTIFY response** (the GIP capability
descriptor). We don't answer it yet, so it retries 4× and powers down.

### The IDENTIFY response descriptor (what Windows wants next)
Payload = 34-byte header then length-prefixed sections:
```
struct gip_pkt_identify {
  u8 unknown[16];
  __le16 client_commands_offset;   // items 24B each
  __le16 firmware_versions_offset; // items 4B each
  __le16 audio_formats_offset;     // items 2B each
  __le16 capabilities_out_offset;  // items 1B each (offset 0 => none)
  __le16 capabilities_in_offset;   // items 1B each
  __le16 classes_offset;           // UTF-16LE strings, le16 len prefix each
  __le16 interfaces_offset;        // GUIDs, 16B each
  __le16 hid_descriptor_offset;    // raw HID bytes
};
```
Each section: `[1-byte count][count × item]`. Offsets are LE16 into the payload; 0 = absent.

**This descriptor is the controller's exact capability identity.** Linux/xone is lenient;
Windows (and the console) validate it. Best captured byte-exact from the real pad via
`gip_probe` rather than hand-rolled. After IDENTIFY, the console/host issues AUTH (0x06),
which needs the genuine chip regardless.

## BREAKTHROUGH: reading the controller needs a host CLASS DRIVER, not raw endpoints
Raw `tuh_edpt_open`/`tuh_edpt_xfer` on the controller's interrupt IN NEVER completed
(`cb=0` forever) — because TinyUSB only activates an interface's endpoints when a class
driver *claims* it. Proof: an MSC pendrive read fine (class driver), our raw approach did not.
Fix: a minimal GIP host class driver registered via `usbh_app_driver_get_cb()` that matches
class FF/47/D0, opens the interrupt endpoints, polls IN, and sends GIP power-on. See
`src/gipdrv/gip_host.c`. Result: controller streams INPUT (0x20) @~125Hz + STATUS (0x03). ✅

### Captured input report (cmd 0x20, 44-byte payload) — Series ctrl 045E:0B12
```
0-1   buttons (bitfield)
2-3   trigger_left  (u16, 0..1023)
4-5   trigger_right (u16, 0..1023)
6-7   stick_left_x  (s16)
8-9   stick_left_y  (s16)
10-11 stick_right_x (s16)
12-13 stick_right_y (s16)
(remaining bytes 0 / padding)
```
STATUS 0x03 payload seen: e.g. `E0 04 8B 01 00 00` (battery/health).

### Debug workflow that unblocked this
FTDI (CP2102N) on GP0/GP1 @115200 for live logs; firmware watches UART for byte 'b' →
`reset_usb_boot()` = button-free BOOTSEL. Flash loop: `picotool` over native USB while
reading COM12. This made protocol iteration ~1 min instead of ~5.

## CAPTURED raw descriptors — real Xbox Series controller 045E:0B12 (donor)
Captured via `gip_probe` over serial. These are the EXACT bytes to clone on the device side.

**Device descriptor (18 bytes):**
```
12 01 00 02 FF 47 D0 40 5E 04 12 0B 09 05 01 02 03 01
```
bcdUSB 0x0200 · class/sub/proto FF/47/D0 · bMaxPacketSize0 0x40 · VID 0x045E · PID 0x0B12 ·
**bcdDevice 0x0509** · iMfr 1 · iProd 2 · iSerial 3 · 1 config.

**Config descriptor (119 bytes, wTotalLength 0x77, 3 interfaces, 500mA):**
```
09 02 77 00 03 01 00 A0 FA
09 04 00 00 02 FF 47 D0 00  07 05 02 03 40 00 04  07 05 82 03 40 00 04   IF0 alt0: GIP data, int EP 0x02/0x82
09 04 00 01 02 FF 47 D0 00  07 05 02 03 40 00 04  07 05 82 03 40 00 02   IF0 alt1
09 04 01 00 00 FF 47 D0 00                                               IF1 alt0: 0 EP
09 04 01 01 02 FF 47 D0 00  07 05 03 01 E4 00 01  07 05 83 01 40 00 01   IF1 alt1: audio iso 0x03/0x83
09 04 02 00 00 FF 47 D0 00                                               IF2 alt0: 0 EP
09 04 02 01 02 FF 47 D0 00  07 05 04 02 40 00 00  07 05 84 02 40 00 00   IF2 alt1: bulk 0x04/0x84
```

**Strings:** 0 = langid 0x0409 · 1 = "Microsoft" · 2 = "Controller" ·
3 = serial "3039373130393232343133303330" (28 ASCII digits).
**BOS descriptor: NONE** (controller STALLs the BOS request → no MS OS 2.0).
**String 0xEE (MS OS 1.0): NOT YET CAPTURED** — this is the current lead (see STATUS.md).

## DualSense BT input report (what `bt_cb` receives)
BT interrupt channel, report id `0x31`; the standard input body starts at **data+3** (63 bytes),
matching the USB report-0x01 layout:
```
[0] LX  [1] LY  [2] RX  [3] RY  [4] L2  [5] R2  [6] seq
[7] buttons0: lo-nibble = dpad hat (0..7, 8=neutral); b4 square b5 cross b6 circle b7 triangle
[8] buttons1: b0 L1 b1 R1 b2 L2 b3 R2 b4 create b5 options b6 L3 b7 R3
[9] buttons2: b0 PS b1 touchpad ...
[~52] battery (low nibble * 10 ≈ %%)
```

## DualSense → Xbox GIP input mapping (`src/passthru/ds_to_gip.h`)
cross→A, circle→B, square→X, triangle→Y, L1→LB, R1→RB, L3→LS, R3→RS, options→menu,
create→view, dpad hat→dpad bits. L2/R2 (0..255)→GIP triggers (0..1023). Sticks 0..255→s16
(Y axes inverted). GIP input report = `20 00 <seq> 0E` + 14-byte payload
(buttons u16, trigL u16, trigR u16, LX/LY/RX/RY s16).

## GIP rumble → DualSense (`passthru.cpp ds_send_feedback`)
GIP rumble 0x09 payload: `[0]?[1]motors[2]Ltrig[3]Rtrig[4]Lmain[5]Rmain`. Mapped to a DualSense
0x31 output report (bt_write): main motors → rumble bytes; impulse triggers → adaptive-trigger
vibration effect (offsets ~b[11]/b[22], mode 0x26 — needs tuning with a game).

## Status vs plan
Phase 0 (PIO-USB host enumerates a device) — **DONE** on hardware.
Phase 1 (read the controller over GIP) — **DONE**: input + status streaming via class driver.
Next: capture announce+identify on a fresh connect (clone descriptor), then device side + auth relay.
Device side (milestone 3 experiment): answered IDENTIFY with a minimal descriptor.
Result: Windows **stopped retrying** identify (framing accepted!) then sent
**POWER 0x04 = GIP_PWR_OFF** — it dismisses the device because the identify content is
hollow (no input classes / interface GUIDs / HID descriptor). No AUTH (0x06) is reached;
Windows rejects on the incomplete capability descriptor first.

**Verified mechanics:** USB enumerate ✓, announce ✓, identify request/response framing ✓,
power/status handling ✓. **Last device-side blocker:** a complete, valid capability
descriptor — large (needs GIP chunked transfer) and controller-specific. Best path:
capture it byte-exact from the real pad via `gip_probe`, not hand-roll it. Then AUTH.
Next: `gip_probe` to capture the controller's descriptors + GIP input bytes (needs controller).
See `docs/XBOX_PASSTHROUGH.md` for the full phase plan.
