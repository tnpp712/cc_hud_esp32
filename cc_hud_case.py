"""
cc_hud enclosure — parametric build123d source (v2).

v2 changes vs v1:
  • Screen dimensions remeasured:
        - active area     35 × 33  (was 32.7 × 35.3 in v1)
        - module PCB      44 × 34  (was 33 × 44 — 90° rotation, the
                                    layout has been re-laid-out)
        - combined thickness 4.2 mm (active + PCB combined; this module
                                     has no separate header-pin
                                     protrusion, so the v1 HEADER_HEIGHT
                                     is gone)
  • Case grows to 48 × 56 × 55 mm. Depth bumped from v1's 30 mm because
    a 35 × 52 LiPo cell + TP4056 + MT3608 + slide switch + cabling do
    not all fit in 30 mm of Z. The CASE_D parameter is now generous so
    you have plenty of room to tuck everything in.
  • Screen-PCB retention switched from 4 corner hooks to two full-width
    beams across the top and bottom of the screen PCB. The PCB X edge
    is now flush with the inner wall, so there's no room for corner
    hooks; the beams cross the PCB Y edge from the wall side instead.
  • Standoff redesigned: WIRE_GAP between the screen PCB back face and
    the ESP32 PCB front face is 3 mm (down from v1's 5 mm), so the
    physical standoff height is 4.2 mm (screen) + 3 mm (gap) = 7.2 mm
    (down from v1's 8.6 mm). The "4 mm column too tall" feedback was
    really about overall depth + standoff together; with the bigger
    case, even a 7.2 mm standoff leaves ~46 mm of Z behind the ESP32
    board for battery + power electronics.

Origin: geometric centre of the case footprint, +X right, +Y up
(USB-C side), +Z back. Front face at Z=0, open back at Z=CASE_D.

Z layering:
    0    .. 2      front panel
    2    .. 6.2    screen module (4.2 mm: active + PCB)
    6.2  .. 9.2    wire gap behind the screen PCB (3 mm)
    9.2  .. 10.8   ESP32-S3 board PCB
    10.8 .. ~16    ESP32 components + USB-C body
    ~16  .. ~21    LiPo cell (~5 mm thick)
    ~21 .. 53      TP4056 + MT3608 + switch + cabling (~32 mm of slack)
    53   .. 55     back cover lip
"""

from build123d import Align, Box, Cylinder, Pos


# ── outer shell ─────────────────────────────────────────────────
CASE_W = 48.0   # X — fits 44 mm screen PCB + 4 mm walls
CASE_H = 56.0   # Y — fits 52 mm LiPo cell + 4 mm walls
CASE_D = 55.0   # Z — generous depth for battery + power module stack
WALL   = 2.0
FRONT  = 2.0

INNER_W = CASE_W - 2 * WALL   # 44
INNER_H = CASE_H - 2 * WALL   # 52


# ── screen ──────────────────────────────────────────────────────
SCREEN_W       = 35.0   # active area, X
SCREEN_H       = 33.0   # active area, Y
SCREEN_PCB_W   = 44.0
SCREEN_PCB_H   = 34.0
SCREEN_TOTAL_T = 4.2    # active + PCB combined
SCREEN_PCB_T   = 1.5

# Retention beams behind the screen PCB's top and bottom edges.
BEAM_X_SIZE   = SCREEN_PCB_W
BEAM_OVERLAP  = 2.0      # how far the beam reaches into the PCB outline
BEAM_Y_SIZE   = (INNER_H - SCREEN_PCB_H) / 2 + BEAM_OVERLAP   # 11
BEAM_Z_THICK  = 2.0
BEAM_Y_CENTER = INNER_H / 2 - BEAM_Y_SIZE / 2                  # 20.5


# ── ESP32 board ─────────────────────────────────────────────────
ESP_PCB_W = 32.0
ESP_PCB_H = 45.0
ESP_PCB_T = 1.6
WIRE_GAP  = 3.0    # screen PCB back → ESP32 PCB front

STANDOFF_D     = 3.0
STANDOFF_INSET = 1.5


# ── USB-C cutout (top wall) ─────────────────────────────────────
USB_W = 8.8
USB_H = 3.3
USB_CLEARANCE = 1.0


# ── bottom feet ─────────────────────────────────────────────────
FOOT_W     = 3.0
FOOT_H     = 3.0
FOOT_DEPTH = 3.0
FOOT_INSET = 3.0


# ── derived Z positions ─────────────────────────────────────────
SCREEN_PCB_Z_BACK = FRONT + SCREEN_TOTAL_T          # 6.2
ESP_PCB_Z_FRONT   = SCREEN_PCB_Z_BACK + WIRE_GAP    # 7.2
ESP_PCB_Z_BACK    = ESP_PCB_Z_FRONT + ESP_PCB_T     # 8.8


def gen_step():
    # 1. Outer shell, Z=0..CASE_D.
    case = Box(CASE_W, CASE_H, CASE_D,
               align=(Align.CENTER, Align.CENTER, Align.MIN))

    # 2. Inner cavity. Overshoots past the back so the back stays open.
    cavity = Box(INNER_W, INNER_H, CASE_D - FRONT + 1.0,
                 align=(Align.CENTER, Align.CENTER, Align.MIN))
    case -= Pos(0, 0, FRONT) * cavity

    # 3. Screen viewport through the front panel.
    sw = Box(SCREEN_W, SCREEN_H, FRONT + 1.0,
             align=(Align.CENTER, Align.CENTER, Align.MIN))
    case -= Pos(0, 0, -0.5) * sw

    # 4. USB-C cutout through the top wall.
    usb = Box(USB_W + USB_CLEARANCE,
              WALL + 2.0,
              USB_H + USB_CLEARANCE,
              align=(Align.CENTER, Align.CENTER, Align.CENTER))
    usb_y = CASE_H / 2 - WALL / 2
    usb_z = ESP_PCB_Z_BACK + USB_H / 2
    case -= Pos(0, usb_y, usb_z) * usb

    # 5. Two retention beams across the X width, one each behind the top
    #    and bottom of the screen PCB. Each beam stretches from the inner
    #    Y wall inward to BEAM_OVERLAP mm inside the PCB outline; they
    #    sit immediately behind the back face of the screen PCB so the
    #    PCB cannot drift toward the back of the case.
    for sy in (-1, 1):
        beam = Box(BEAM_X_SIZE, BEAM_Y_SIZE, BEAM_Z_THICK)
        case += Pos(0, sy * BEAM_Y_CENTER,
                     SCREEN_PCB_Z_BACK + BEAM_Z_THICK / 2) * beam

    # 6. ESP32 standoffs — four cylinders rising from the front inner
    #    wall to the front face of the ESP32 board. They sit in the
    #    PCB-corner safe zone (just inside each corner by STANDOFF_INSET).
    standoff_h = ESP_PCB_Z_FRONT - FRONT   # 7.2 mm
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx = sx * (ESP_PCB_W / 2 - STANDOFF_INSET)
            cy = sy * (ESP_PCB_H / 2 - STANDOFF_INSET)
            so = Cylinder(STANDOFF_D / 2, standoff_h,
                          align=(Align.CENTER, Align.CENTER, Align.MIN))
            case += Pos(cx, cy, FRONT) * so

    # 7. Four feet hanging below the bottom face.
    fy = -CASE_H / 2 - FOOT_H / 2
    for sx in (-1, 1):
        for sz in (0, 1):
            fx = sx * (CASE_W / 2 - FOOT_INSET - FOOT_W / 2)
            fz_span = CASE_D - 2 * FOOT_INSET - FOOT_DEPTH
            fz = sz * fz_span + FOOT_INSET + FOOT_DEPTH / 2
            case += Pos(fx, fy, fz) * Box(FOOT_W, FOOT_H, FOOT_DEPTH)

    return case
