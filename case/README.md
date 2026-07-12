# DS5Dongle Case (v2 — compact)

Two-part press-fit enclosure for the DS5Dongle (Raspberry Pi Pico 2W + 0.91" 128x32
SSD1306 OLED + snap-in panel USB-A female pigtail). Designed in Fusion, exported here
as STLs. Outer size: 60 x 30 x 24.5 mm.

The snap-in USB connector's 18 mm body sits *above* the far end of the Pico
(stacked layout), which is what keeps the case short. The OLED sits toward the
micro-USB end, clearing the connector body.

## Files

- `ds5dongle_casebottom.stl` — bottom shell (print as-is, open side up)
- `ds5dongle_caselid.stl` — lid (flip 180° so the flat top faces the bed)

## Print settings

- 0.2 mm layers, 2+ perimeters, no supports needed (both parts print flat;
  the USB cutout bridges ~20 mm — any printer handles that)
- PETG or PLA. Press fit comes from 6 tapered crush bumps on the lid rib
  (4 on the long sides, 2 on the ends; ~0.3 mm interference each, ~0.15 mm/side
  clearance elsewhere) — sand the bumps lightly if too tight

## Fit reference

| Feature | Dimension |
|---|---|
| Pico 2W mounts | 4 posts, Ø5 standoff 3 mm tall (all equal — board sits level). Rear pins Ø1.9 x 3 mm; front pins (next to micro-USB) shortened to 2 mm incl. taper so the board tilts in past them without enlarging the micro-USB hole |
| Micro-USB opening | 9 x 4.2 mm outside (plug shell only passes; overmold butts against the wall). Inside top edge has a 45° lead-in ramp (opening grows to ~6.2 mm tall at the inner face) so the connector enters easily with the board inclined |
| USB-A snap-in cutout | 20.4 x 10.4 mm, raised — lower edge ~9 mm above the case floor so the body (20 x 10 x 18 mm, RUNCCI-YUN) rides over the Pico with ~1 mm clearance |
| OLED window | 25 x 9 mm through the lid, toward the micro-USB end |
| OLED retention | 4 snap tabs with round lips (relaxed fit: lips catch flush under the PCB, ~0.1 mm side clearance) + end wall at the micro-USB end; the USB connector body backs up the other end. Gentle press-fit — add a dab of hot glue to lock it |
| Lid retention | 3 mm perimeter rib, pressure fit into the bottom shell |

## Assembly (order matters)

1. Solder the pigtail wires to the Pico first — D+ = GP2, D- = GP3, plus
   VBUS/GND (all pads are at the micro-USB end, clear of the connector body).
2. Mount the Pico: slide the micro-USB connector into its wall opening at a
   shallow angle, then lower the rear of the board onto the two rear pins.
   **Do this before the USB connector** — the connector body covers the rear posts.
3. Snap the USB-A pigtail connector into the raised wall cutout from outside;
   the clip catches inside the 2 mm wall. Hot glue if loose.
4. Clip the OLED face-down into the lid (pins toward the USB-A end / wire gap).
   Hot glue the corners if the module sits loose.
5. Dress the wires beside the connector body and press the lid on. Fingernail
   notch on the front long wall pries it open again.
