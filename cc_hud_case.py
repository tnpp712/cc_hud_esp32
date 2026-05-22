"""
cc_hud enclosure — parametric build123d source.

Single-piece print, open at the back so PCBs are loaded after printing.

Coordinate system (mm):
    Origin       = geometric centre of the case footprint
    +X / -X      = right / left
    +Y / -Y      = top (USB-C side) / bottom (feet side)
    +Z           = depth (front panel at Z=0, open back at Z=CASE_D)

Internal layering along +Z (from front to back):
    Z = 0 .. FRONT                    front panel (2 mm)
    Z = FRONT .. FRONT+SCREEN_PCB_T   screen PCB body
    Z = .. + HEADER_HEIGHT            screen back header pins clearance
    Z = .. + WIRE_GAP                 free space for the screen→ESP32 wires
    Z = ESP_PCB_Z_FRONT..             ESP32-S3 board
    Z = ESP_PCB_Z_BACK..CASE_D        USB-C body + wire fan-out, open back

Features:
    • Screen viewport cut out of the front panel.
    • 4 corner hooks behind the screen PCB locking it against the front panel.
    • 4 standoffs lifting the ESP32 board off the front panel.
    • USB-C cutout in the top wall, aligned to the ESP32 board surface.
    • 4 feet under the bottom face (lift the case for cables / airflow).
    • Back face is fully open so PCBs can be inserted after printing.

Generate with the CAD skill:
    PYTHONPATH=~/.claude/skills/cad/scripts \\
        python3 -m step cc_hud_case.py --3mf cc_hud_case.3mf
"""

from build123d import (
    Align,
    Box,
    Cylinder,
    Pos,
)


# ── outer shell ─────────────────────────────────────────────────
CASE_W = 38.0   # X
CASE_H = 50.0   # Y
CASE_D = 30.0   # Z (user-specified)
WALL   = 2.0
FRONT  = 2.0

INNER_W = CASE_W - 2 * WALL   # 34
INNER_H = CASE_H - 2 * WALL   # 46


# ── screen ──────────────────────────────────────────────────────
SCREEN_W      = 32.7
SCREEN_H      = 35.3
SCREEN_PCB_W  = 33.0
SCREEN_PCB_H  = 44.0
SCREEN_PCB_T  = 1.6     # estimated PCB thickness
HEADER_HEIGHT = 2.0     # back-side header pin clearance

# Hooks behind the screen PCB at the four inner corners.
HOOK_SIZE  = 2.5
HOOK_THICK = 2.0


# ── ESP32 board ─────────────────────────────────────────────────
ESP_PCB_W = 32.0
ESP_PCB_H = 45.0
ESP_PCB_T = 1.6
WIRE_GAP  = 5.0

STANDOFF_D     = 3.0   # standoff column outer diameter
STANDOFF_INSET = 1.5   # how far in from each PCB corner the standoff sits


# ── USB-C cutout (top wall) ─────────────────────────────────────
USB_W = 8.8
USB_H = 3.3
USB_CLEARANCE = 1.0


# ── bottom feet ─────────────────────────────────────────────────
FOOT_W     = 3.0
FOOT_H     = 3.0
FOOT_DEPTH = 3.0
FOOT_INSET = 3.0   # distance from case X / Z edge to foot centre


# ── derived Z positions (do not edit) ───────────────────────────
SCREEN_PCB_Z_BACK = FRONT + SCREEN_PCB_T            # 3.6
HEADER_Z_BACK     = SCREEN_PCB_Z_BACK + HEADER_HEIGHT  # 5.6
ESP_PCB_Z_FRONT   = HEADER_Z_BACK + WIRE_GAP        # 10.6
ESP_PCB_Z_BACK    = ESP_PCB_Z_FRONT + ESP_PCB_T     # 12.2


def gen_step():
    # 1. Outer shell. Front face anchored to Z=0 (Align.MIN on Z).
    case = Box(CASE_W, CASE_H, CASE_D,
               align=(Align.CENTER, Align.CENTER, Align.MIN))

    # 2. Inner cavity — overshoots past the back so it stays open.
    cavity = Box(INNER_W, INNER_H, CASE_D - FRONT + 1.0,
                 align=(Align.CENTER, Align.CENTER, Align.MIN))
    case -= Pos(0, 0, FRONT) * cavity

    # 3. Screen viewport through the front panel.
    sw = Box(SCREEN_W, SCREEN_H, FRONT + 1.0,
             align=(Align.CENTER, Align.CENTER, Align.MIN))
    case -= Pos(0, 0, -0.5) * sw

    # 4. USB-C cutout through the top wall.
    #    Centred on X; punches right through the top wall in Y; in Z the cut
    #    is centred on the USB-C body which sits just above the ESP32 PCB
    #    upper face.
    usb = Box(USB_W + USB_CLEARANCE,
              WALL + 2.0,
              USB_H + USB_CLEARANCE,
              align=(Align.CENTER, Align.CENTER, Align.CENTER))
    usb_y = CASE_H / 2 - WALL / 2
    usb_z = ESP_PCB_Z_BACK + USB_H / 2
    case -= Pos(0, usb_y, usb_z) * usb

    # 5. Screen-PCB retainer hooks at the four inner corners. They sit
    #    flush against the side walls and protrude into the cavity right
    #    behind the screen PCB, locking it against the front panel.
    hook_z = SCREEN_PCB_Z_BACK + HOOK_THICK / 2
    for sx in (-1, 1):
        for sy in (-1, 1):
            hx = sx * (INNER_W / 2 - HOOK_SIZE / 2)
            hy = sy * (INNER_H / 2 - HOOK_SIZE / 2)
            case += Pos(hx, hy, hook_z) * Box(HOOK_SIZE, HOOK_SIZE, HOOK_THICK)

    # 6. ESP32 standoffs — four cylinders rising from the front inner wall
    #    up to the front face of the ESP32 board.
    standoff_h = ESP_PCB_Z_FRONT - FRONT
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
