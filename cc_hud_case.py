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

Z layering:
    0    .. 2      front panel
    2    .. 6.2    screen module (4.2 mm: active + PCB)
    6.2  .. 9.2    wire gap behind the screen PCB (3 mm)
    9.2  .. 10.8   ESP32-S3 board PCB
    10.8 .. ~16    ESP32 components + USB-C body
    ~16  .. ~21    LiPo cell (~5 mm thick)
    ~21  .. 53     TP4056 + MT3608 + slide switch + cabling (~32 mm slack)
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
FRONT  = 2.0

INNER_W = CASE_W - 2 * WALL   # 36
INNER_H = CASE_H - 2 * WALL   # 54


# ── screen (portrait) ───────────────────────────────────────────
SCREEN_W       = 33.0   # active area, X (short edge)
SCREEN_H       = 35.0   # active area, Y (long edge)
SCREEN_PCB_W   = 34.0
SCREEN_PCB_H   = 44.0
SCREEN_TOTAL_T = 4.2    # active + PCB combined
SCREEN_PCB_T   = 1.5

# Corner L-bracket hooks behind the four screen-PCB corners. Each hook
# straddles the PCB X edge and the PCB Y edge from the inner-wall side
# so the PCB is locked against the front panel from behind.
HOOK_X       = 4.0
HOOK_Y       = 8.0   # spans the 5 mm Y wall-to-PCB gap + 3 mm into the PCB
HOOK_Z_THICK = 2.0


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
ESP_PCB_Z_FRONT   = SCREEN_PCB_Z_BACK + WIRE_GAP    # 9.2
ESP_PCB_Z_BACK    = ESP_PCB_Z_FRONT + ESP_PCB_T     # 10.8


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

    # 5. Four corner L-bracket hooks behind the screen PCB. Each one is
    #    flush with the inner wall on both X and Y, so it grabs the PCB
    #    corner from the back and pins it against the front panel.
    hook_x_center = INNER_W / 2 - HOOK_X / 2
    hook_y_center = INNER_H / 2 - HOOK_Y / 2
    hook_z_center = SCREEN_PCB_Z_BACK + HOOK_Z_THICK / 2
    for sx in (-1, 1):
        for sy in (-1, 1):
            hook = Box(HOOK_X, HOOK_Y, HOOK_Z_THICK)
            case += Pos(sx * hook_x_center,
                        sy * hook_y_center,
                        hook_z_center) * hook

    # 6. ESP32 standoffs — four cylinders rising from the front inner
    #    wall to the front face of the ESP32 board.
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
