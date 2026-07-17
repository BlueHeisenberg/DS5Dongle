//
// DualSense input report -> Xbox GIP gamepad input report mapping.
//
// Input: the DualSense report body (the 63-byte block the BT link delivers,
// i.e. data+3 of the 0x31 report — same buffer the original firmware fills).
//   [0] LX  [1] LY  [2] RX  [3] RY  [4] L2  [5] R2  [6] seq
//   [7] buttons0: lo nibble = dpad hat (0..7, 8=neutral);
//                 b4 square, b5 cross, b6 circle, b7 triangle
//   [8] buttons1: b0 L1, b1 R1, b2 L2, b3 R2, b4 create, b5 options, b6 L3, b7 R3
//   [9] buttons2: b0 PS, b1 touchpad, ...
//
// Output: 14-byte GIP input payload (see docs/GIP_NOTES.md):
//   [0..1] buttons  [2..3] trigL  [4..5] trigR
//   [6..7] LX  [8..9] LY  [10..11] RX  [12..13] RY   (little-endian)
//
#pragma once
#include <stdint.h>
#include <string.h>

// GIP button bits (byte0/byte1 of the report, as a u16)
enum {
    GIP_BTN_SYNC  = 1u << 0,
    GIP_BTN_MENU  = 1u << 2,   // start / options
    GIP_BTN_VIEW  = 1u << 3,   // back / create
    GIP_BTN_A     = 1u << 4,
    GIP_BTN_B     = 1u << 5,
    GIP_BTN_X     = 1u << 6,
    GIP_BTN_Y     = 1u << 7,
    GIP_BTN_DUP   = 1u << 8,
    GIP_BTN_DDOWN = 1u << 9,
    GIP_BTN_DLEFT = 1u << 10,
    GIP_BTN_DRIGHT= 1u << 11,
    GIP_BTN_LB    = 1u << 12,
    GIP_BTN_RB    = 1u << 13,
    GIP_BTN_LS    = 1u << 14,
    GIP_BTN_RS    = 1u << 15,
};

static inline void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline void put_s16(uint8_t *p, int16_t v)   { put_le16(p, (uint16_t) v); }

// Scale an unsigned 8-bit axis (0..255, 128 center) to signed 16-bit.
// invert=true flips direction (DualSense Y is up=0, Xbox Y is up=positive).
static inline int16_t axis8_to_s16(uint8_t v, bool invert) {
    int centered = (int) v - 128;           // -128..127
    if (invert) centered = -centered - 1;    // keep symmetric
    int scaled = centered * 258;             // ~ -33024..32766
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t) scaled;
}

// 8-bit trigger (0..255) -> GIP 10-bit (0..1023)
static inline uint16_t trig8_to_gip(uint8_t v) { return (uint16_t) ((v * 1023) / 255); }

// Map a DualSense report body -> 14-byte GIP input payload.
static inline void dualsense_to_gip_input(const uint8_t *ds, uint8_t out[14]) {
    memset(out, 0, 14);

    const uint8_t b0 = ds[7];   // dpad + face
    const uint8_t b1 = ds[8];   // shoulders / sticks / menu
    const uint8_t b2 = ds[9];   // PS / touchpad

    uint16_t btn = 0;
    if (b0 & (1u << 5)) btn |= GIP_BTN_A;   // cross  -> A
    if (b0 & (1u << 6)) btn |= GIP_BTN_B;   // circle -> B
    if (b0 & (1u << 4)) btn |= GIP_BTN_X;   // square -> X
    if (b0 & (1u << 7)) btn |= GIP_BTN_Y;   // triangle -> Y
    if (b1 & (1u << 0)) btn |= GIP_BTN_LB;  // L1
    if (b1 & (1u << 1)) btn |= GIP_BTN_RB;  // R1
    if (b1 & (1u << 6)) btn |= GIP_BTN_LS;  // L3
    if (b1 & (1u << 7)) btn |= GIP_BTN_RS;  // R3
    if (b1 & (1u << 5)) btn |= GIP_BTN_MENU;// options -> menu
    if (b1 & (1u << 4)) btn |= GIP_BTN_VIEW;// create  -> view
    if (b2 & (1u << 0)) btn |= GIP_BTN_SYNC;// PS -> guide handled separately on real HW

    switch (b0 & 0x0F) {                    // dpad hat
        case 0: btn |= GIP_BTN_DUP; break;
        case 1: btn |= GIP_BTN_DUP | GIP_BTN_DRIGHT; break;
        case 2: btn |= GIP_BTN_DRIGHT; break;
        case 3: btn |= GIP_BTN_DDOWN | GIP_BTN_DRIGHT; break;
        case 4: btn |= GIP_BTN_DDOWN; break;
        case 5: btn |= GIP_BTN_DDOWN | GIP_BTN_DLEFT; break;
        case 6: btn |= GIP_BTN_DLEFT; break;
        case 7: btn |= GIP_BTN_DUP | GIP_BTN_DLEFT; break;
        default: break;                     // 8 = neutral
    }

    put_le16(out + 0, btn);
    put_le16(out + 2, trig8_to_gip(ds[4]));         // L2 -> trigger left
    put_le16(out + 4, trig8_to_gip(ds[5]));         // R2 -> trigger right
    put_s16 (out + 6,  axis8_to_s16(ds[0], false)); // LX
    put_s16 (out + 8,  axis8_to_s16(ds[1], true));  // LY (invert)
    put_s16 (out + 10, axis8_to_s16(ds[2], false)); // RX
    put_s16 (out + 12, axis8_to_s16(ds[3], true));  // RY (invert)
}
