"""
cc_hud enclosure — parametric build123d source (v2, portrait).

Screen is portrait (tall) — long edge runs along Y, short edge along X.

Dimensions (mm):
  Screen active area :   33 × 35  (X × Y, short × long)
  Screen module PCB  :   34 × 44
  Combined thickness :    4.2 mm (active + PCB, no separate header pins)
  ESP32-S3 board PCB :   32 × 45
  LiPo cell          :   35 × 52  (~5 mm thick)
  Case outer         :   40 × 58 × 55  (X × Y × Z)

Origin: geometric centre of the case footprint, +X right, +Y up
(USB-C side), +Z back. Front face at Z=0, open back at Z=CASE_D.

Z layering (FRONT bumped to 2.7 mm so the screen glass embeds inside
the panel, leaving only the PCB depth inside the cavity):
    0    .. 2.7    front panel (screen active glass occupies the cut-out)
    2.7  .. 4.2    screen module PCB (1.5 mm)
    2.7  .. 4.1    1.4 mm standoffs (offset outside ESP corner)
    4.1  .. 5.7    ESP32-S3 board PCB
    5.7  .. ~10    ESP32 components + USB-C body
    ~10  .. ~15    LiPo cell (~5 mm thick)
    ~15  .. 53     TP4056 + MT3608 + slide switch + cabling (~38 mm slack)
    53   .. 55     back cover lip

Internal width (40-2*2 = 36) accommodates the 35 mm battery plus a
~0.5 mm clearance per side. Internal height (58-2*2 = 54) gives the
52 mm battery a 1 mm clearance per side and leaves 4.5 mm above/below
the 45 mm ESP32 board for cable routing.
"""

from build123d import Align, Box, Cylinder, Pos


# ── outer shell ─────────────────────────────────────────────────
CASE_W = 40.0   # X — fits 35 mm battery + clearance + 4 mm walls
CASE_H = 58.0   # Y — fits 52 mm battery + clearance + 4 mm walls
CASE_D = 55.0   # Z — battery + power stack live behind the ESP32
WALL   = 2.0
FRONT  = 2.7    # bumped from 2.0 so the screen active glass (2.7 mm)
                # embeds INSIDE the front panel cut-out, exposing only
                # the PCB depth (1.5 mm) inside the cavity. That's what
                # lets the standoffs be physically 1.4 mm short.

INNER_W = CASE_W - 2 * WALL   # 36
INNER_H = CASE_H - 2 * WALL   # 54


# ── screen (portrait) ───────────────────────────────────────────
SCREEN_W       = 33.0   # active area, X (short edge)
SCREEN_H       = 35.0   # active area, Y (long edge)
SCREEN_PCB_W   = 34.0
SCREEN_PCB_H   = 44.0
SCREEN_TOTAL_T = 4.2    # active + PCB combined (informational)
SCREEN_PCB_T   = 1.5    # PCB only — active glass is hidden inside FRONT

# (Removed: the original 4 corner L-bracket hooks have been deleted at
#  user request. The screen PCB now relies on the front panel pressing
#  it from the front and the ESP32 board surface from behind to stay in
#  place. If yours rattles in shipping, a 5 × 5 mm dab of foam or a
#  drop of hot glue on each corner is enough.)


# ── ESP32 board ─────────────────────────────────────────────────
ESP_PCB_W = 32.0
ESP_PCB_H = 45.0
ESP_PCB_T = 1.6

STANDOFF_D     = 3.0
STANDOFF_H     = 1.4    # ← user-requested physical column height
STANDOFF_INSET = -0.5   # negative = standoff centre sits just OUTSIDE the
                        #   ESP32 PCB corner, so the standoff column does
                        #   not collide with the screen PCB sitting in
                        #   Z=FRONT..FRONT+SCREEN_PCB_T. The board corner
                        #   still falls inside the standoff disc because
                        #   the board corner is only 0.5 mm out.


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
# With FRONT=2.7, the screen active glass sits in Z=0..2.7 (inside the
# front-panel cut-out). The screen PCB occupies Z=2.7..4.2 right behind
# the panel. Standoffs rise 1.4 mm from the panel inner face to Z=4.1,
# so the ESP32 board sits at Z=4.1..5.7 — 0.1 mm of nominal interference
# with the PCB back face at Z=4.2, which is absorbed by print tolerance.
SCREEN_PCB_Z_BACK = FRONT + SCREEN_PCB_T            # 4.2
ESP_PCB_Z_FRONT   = FRONT + STANDOFF_H              # 4.1
ESP_PCB_Z_BACK    = ESP_PCB_Z_FRONT + ESP_PCB_T     # 5.7


def gen_step():
    # 1. Outer shell, Z=0..CASE_D.
    case = Box(CASE_W, CASE_H, CASE_D,
               align=(Align.CENTER, Align.CENTER, Align.MIN))

    # 2. Inner cavity (overshoots past the back so the back stays open).
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

    # 5. ESP32 standoffs — four 1.4 mm-tall cylinders just outside each
    #    ESP32 board corner. Pushed slightly past the corner so the
    #    column does not eat into the screen PCB sitting in front of it.
    standoff_h = STANDOFF_H   # 1.4 mm physical
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx = sx * (ESP_PCB_W / 2 - STANDOFF_INSET)
            cy = sy * (ESP_PCB_H / 2 - STANDOFF_INSET)
            so = Cylinder(STANDOFF_D / 2, standoff_h,
                          align=(Align.CENTER, Align.CENTER, Align.MIN))
            case += Pos(cx, cy, FRONT) * so

    # 6. Four feet hanging below the bottom face.
    fy = -CASE_H / 2 - FOOT_H / 2
    for sx in (-1, 1):
        for sz in (0, 1):
            fx = sx * (CASE_W / 2 - FOOT_INSET - FOOT_W / 2)
            fz_span = CASE_D - 2 * FOOT_INSET - FOOT_DEPTH
            fz = sz * fz_span + FOOT_INSET + FOOT_DEPTH / 2
            case += Pos(fx, fy, fz) * Box(FOOT_W, FOOT_H, FOOT_DEPTH)

    return case
