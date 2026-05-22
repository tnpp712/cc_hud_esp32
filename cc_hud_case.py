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
    0    .. 2.7    front panel (screen active glass occupies the cut-out;
                                solder-joint relief pocket carved 1.5 mm
                                deep into the inner face under the
                                screen PCB's bottom-edge header row)
    2.7  .. 4.2    screen module PCB (1.5 mm)
    4.2  .. 5.8    ESP32-S3 board PCB (tape / glue directly behind the
                                       screen PCB — no standoffs)
    5.8  .. ~11    ESP32 components + USB-C body
    ~11  .. ~16    LiPo cell (~5 mm thick)
    ~16  .. 53     TP4056 + MT3608 + slide switch + cabling
    53   .. 55     back cover lip

Internal width (40-2*2 = 36) accommodates the 35 mm battery plus a
~0.5 mm clearance per side. Internal height (58-2*2 = 54) gives the
52 mm battery a 1 mm clearance per side and leaves 4.5 mm above/below
the 45 mm ESP32 board for cable routing.
"""

from build123d import Align, Box, Pos


# ── outer shell ─────────────────────────────────────────────────
CASE_W = 40.0   # X — fits the 32 mm ESP32 board (upright) + walls
CASE_H = 58.0   # Y — fits 52 mm battery + clearance + 4 mm walls
CASE_D = 55.0   # Z — battery + power stack live behind the ESP32
WALL   = 2.0
FRONT  = 2.7    # screen active glass (2.7 mm) embeds INSIDE the front
                # panel cut-out; only the 1.5 mm PCB sits behind the
                # panel, leaving room for the 1.4 mm standoffs.

# NOTE: the USB-C charging port now exits through the BACK COVER, not
# the case. Use a 90° USB-C extension cable inside the cavity, or a
# USB-C panel-mount socket secured behind the back-cover hole.

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


# ── ESP32 board (upright, short edge along X) ───────────────────
ESP_PCB_W = 32.0   # X — short edge
ESP_PCB_H = 45.0   # Y — long edge
ESP_PCB_T = 1.6

# (Standoffs removed at user request. The ESP32 board now rides
#  directly behind the screen PCB; secure it with a 5 × 5 mm dab of
#  double-sided foam tape or a small drop of hot glue at each corner.)


# ── solder-joint relief pocket on the inner face of the front panel ─
# The screen module's header pins / FPC solder pads on the back of the
# PCB stick out roughly 1.5 mm. They live along the BOTTOM edge of the
# screen PCB (the -Y short edge). We carve a shallow pocket into the
# inner face of the front panel under that strip so the screen PCB
# can sit flush against the panel without the pins jamming.
SLOT_X     = 30.0   # how wide the pocket runs across X (pin row span)
SLOT_Y     = 5.0    # depth into the case along Y (crosses the PCB edge)
SLOT_Z     = 1.5    # pocket depth — matches user-quoted pin height
SLOT_Y_POS = -19.5  # Y centre of the pocket (slightly inside the
                    # screen PCB outline so the pins it relieves sit
                    # both inside and just below the PCB edge)


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
# the panel. Without standoffs, the ESP32 board is expected to sit
# right behind the screen PCB (glued or taped); these Z positions are
# documentary only.
SCREEN_PCB_Z_BACK = FRONT + SCREEN_PCB_T            # 4.2
ESP_PCB_Z_FRONT   = SCREEN_PCB_Z_BACK               # 4.2 (touches PCB back)
ESP_PCB_Z_BACK    = ESP_PCB_Z_FRONT + ESP_PCB_T     # 5.8


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

    # 4. (No USB-C cutout on the case body — the charging port now
    #     punches through the back cover. See cc_hud_back_cover.py.)

    # 5. Solder-joint relief pocket — a shallow rectangular cavity on
    #    the inner face of the front panel, sitting where the screen
    #    module's header-pin solder pads would otherwise press against
    #    the panel.
    pocket = Box(SLOT_X, SLOT_Y, SLOT_Z,
                 align=(Align.CENTER, Align.CENTER, Align.MIN))
    case -= Pos(0, SLOT_Y_POS, FRONT - SLOT_Z) * pocket

    # 6. Four feet hanging below the bottom face.
    fy = -CASE_H / 2 - FOOT_H / 2
    for sx in (-1, 1):
        for sz in (0, 1):
            fx = sx * (CASE_W / 2 - FOOT_INSET - FOOT_W / 2)
            fz_span = CASE_D - 2 * FOOT_INSET - FOOT_DEPTH
            fz = sz * fz_span + FOOT_INSET + FOOT_DEPTH / 2
            case += Pos(fx, fy, fz) * Box(FOOT_W, FOOT_H, FOOT_DEPTH)

    return case
