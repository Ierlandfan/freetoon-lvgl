#!/usr/bin/env python3
"""
Generates two C headers:
  icon_flame.h  — 32x40 alpha-8bit, flame teardrop shape (recolor at runtime)
  icon_faucet_drop.h — 40x40 alpha-8bit two-layer (faucet body + red drop) as a
    single recolorable bitmap. Because we want TWO colors (silver body +
    red drop), this file actually emits TWO bitmaps:
      icon_faucet (the faucet body alpha)
      icon_drop   (the drop alpha)
    They will be drawn as two stacked lv_img with different recolor.

Run:
    python3 gen_icons.py
"""

import math

W_FLAME, H_FLAME = 32, 40
W_FAUC, H_FAUC = 40, 40

def write_alpha8(name, arr, w, h, fout):
    """Emit a C header for an alpha-8bit lv_img_dsc_t."""
    data = bytearray(len(arr))
    for i, v in enumerate(arr):
        data[i] = max(0, min(255, int(v)))
    fout.write(f"/* {name}: {w}x{h} alpha-8bit, generated, do not edit */\n")
    fout.write(f"#include \"lvgl/lvgl.h\"\n\n")
    fout.write(f"static const uint8_t {name}_data[] = {{\n")
    for y in range(h):
        row = data[y*w:(y+1)*w]
        fout.write("    " + ", ".join(f"0x{b:02x}" for b in row) + ",\n")
    fout.write("};\n\n")
    fout.write(f"const lv_img_dsc_t {name} = {{\n")
    fout.write(f"    .header.cf = LV_IMG_CF_ALPHA_8BIT,\n")
    fout.write(f"    .header.always_zero = 0,\n")
    fout.write(f"    .header.reserved = 0,\n")
    fout.write(f"    .header.w = {w},\n")
    fout.write(f"    .header.h = {h},\n")
    fout.write(f"    .data_size = sizeof({name}_data),\n")
    fout.write(f"    .data = {name}_data,\n")
    fout.write("};\n")

# --- FLAME shape ---
# Teardrop pointing up. Half-width at row y (0=top, H_FLAME-1=bottom):
#   widens from 1 to ~W_FLAME/2 across upper 75%, then rounds back in.
def flame_alpha(x, y):
    cx = W_FLAME / 2.0 - 0.5
    # vertical normalized position 0..1 from top to bottom
    t = y / (H_FLAME - 1)
    # Clean symmetric flame teardrop: a sharp point at the top, swelling to
    # a rounded belly around t=0.7, then tucking back in at the base. No
    # flicker "wiggle" (it just made the 32px render look lumpy/asymmetric).
    maxhalf = W_FLAME / 2.0 - 1.5
    if t < 0.7:
        # upper body — smooth widening from the tip (slightly more than a
        # straight line so the point reads as a flame, not a triangle)
        half = maxhalf * math.pow(t / 0.7, 0.8)
    else:
        # lower body — round the belly back in toward a flat-ish base
        r = (t - 0.7) / 0.3            # 0 at belly, 1 at base
        half = maxhalf * math.sqrt(max(0.0, 1.0 - 0.55 * r * r))
    d = abs(x - cx) - half
    # Anti-aliased edge: alpha falls linearly from 255 (d<=-1) to 0 (d>=1)
    if d <= -1:    return 255
    if d >=  1:    return 0
    return int((1 - d) * 0.5 * 255)

# --- FAUCET BODY ---
# Side-view faucet: vertical pipe on left, horizontal arm to the right at top,
# then bends down into a spout. (~32 wide x 28 tall, padded with blank around.)
def faucet_alpha(x, y):
    # All coords in image-space (W_FAUC, H_FAUC = 40x40)
    a = 0
    # Vertical pipe on left edge: x in [4,9], y in [4,30]
    if 4 <= x <= 9 and 4 <= y <= 30: a = 255
    # Horizontal arm top: x in [4,30], y in [4,9]
    if 4 <= x <= 30 and 4 <= y <= 9: a = 255
    # Downturn at right end: x in [25,30], y in [4,24]
    if 25 <= x <= 30 and 4 <= y <= 24: a = 255
    # Spout opening (slightly wider square): x in [23,32], y in [22,28]
    if 23 <= x <= 32 and 22 <= y <= 28: a = 255
    # Handle on top: x in [12,20], y in [0,4]
    if 12 <= x <= 20 and 0 <= y <= 4: a = 255
    # Soft AA: 1-pixel border feather
    if a == 0:
        # Check immediate neighbors — if any neighbor is on edge, soften
        return 0
    return a

# --- RED DROP (small teardrop, falls under spout) ---
W_DROP, H_DROP = 14, 18
def drop_alpha(x, y):
    cx = W_DROP / 2.0 - 0.5
    t = y / (H_DROP - 1)
    # Drop shape: pointed at top, round at bottom
    if t < 0.4:
        half = (W_DROP / 2.0 - 1.0) * (t / 0.4)
    else:
        # rounded bottom
        r = (t - 0.4) / 0.6
        half = (W_DROP / 2.0 - 1.0) * math.sqrt(max(0.0, 1.0 - (1.0-r)*(1.0-r)))
    d = abs(x - cx) - half
    if d <= -1: return 255
    if d >=  1: return 0
    return int((1 - d) * 0.5 * 255)

def gen(name, w, h, fn, outfile):
    arr = [fn(x, y) for y in range(h) for x in range(w)]
    with open(outfile, "w") as f:
        write_alpha8(name, arr, w, h, f)

gen("icon_flame",  W_FLAME, H_FLAME, flame_alpha,  "icon_flame.h")
gen("icon_faucet", W_FAUC,  H_FAUC,  faucet_alpha, "icon_faucet.h")
gen("icon_drop",   W_DROP,  H_DROP,  drop_alpha,   "icon_drop.h")

# --- RADIATOR + FLAME (compact 32x28) — Toon-style CH-heating glyph.
# Layout (column ranges in image-space):
#   x  0..9    : small upright flame
#   x 12..31   : 4 vertical radiator fins with top + bottom caps
W_RAD, H_RAD = 32, 28
def radiator_alpha(x, y):
    # Mini-flame on the left, top-aligned, 10x16
    if 0 <= x <= 9 and 4 <= y <= 22:
        fx, fy = x, y - 4
        FW, FH = 10, 18
        cx = FW / 2.0 - 0.5
        t = fy / (FH - 1)
        if t < 0.85:
            half = (FW / 2.0 - 0.5) * math.sqrt(max(0.0, t / 0.85))
        else:
            r = (1.0 - t) / 0.15
            half = (FW / 2.0 - 0.5) * math.sqrt(max(0.0, r))
        half += 0.3 * math.sin(t * math.pi * 3.0)
        d = abs(fx - cx) - half
        if d <= -1: return 255
        if d <   1: return int((1 - d) * 0.5 * 255)
    # Radiator on the right
    R_X0, R_X1 = 12, 31           # body extent
    R_Y0, R_Y1 = 4, 25            # body extent
    if R_X0 <= x <= R_X1 and R_Y0 <= y <= R_Y1:
        # Top + bottom horizontal caps (2 px thick)
        if R_Y0 <= y <= R_Y0 + 1 or R_Y1 - 1 <= y <= R_Y1:
            return 255
        # 4 fins: x positions 13,17,21,25 each 3 px wide → 3,7,11,15 mod 4
        col = x - 13
        if col % 4 == 0 or col % 4 == 1 or col % 4 == 2:
            return 255
    return 0
gen("icon_radiator", W_RAD, H_RAD, radiator_alpha, "icon_radiator.h")
# (icon_fan moved to BGRA emit below — alpha-8 fan + rotation = LVGL 8.3 bug)

# --- FAN (3-blade) — used by the Vent tile, rotated via lv_img_set_angle.
# Square so rotation has no clipping. 80x80 alpha-8.
W_FAN, H_FAN = 80, 80
def fan_alpha(x, y):
    cx = W_FAN / 2.0 - 0.5
    cy = H_FAN / 2.0 - 0.5
    dx = x - cx; dy = y - cy
    r = math.sqrt(dx*dx + dy*dy)
    # outer ring (rim)
    R_OUT = W_FAN / 2.0 - 2.0
    R_IN  = R_OUT - 3.0
    # central hub
    R_HUB = 6.0
    if r <= R_HUB:    return 255
    if R_IN <= r <= R_OUT:
        # rim: anti-alias outer edge
        if r > R_OUT - 1.0:  return int((R_OUT - r) * 255)
        if r < R_IN + 1.0:   return int((r - R_IN) * 255)
        return 255
    if r >= R_OUT: return 0
    # blades: 3 curved blades spaced 120° apart.
    ang = math.atan2(dy, dx) + math.pi   # 0..2pi
    blade = (ang / (2*math.pi/3)) % 1.0   # 0..1 within a 120° wedge
    # blade body is the inner 0..0.55 of each wedge (after a slight curve)
    # curvature: shift wedge boundary by (r/R_OUT)*0.18 → swept-back look
    bcurve = blade - (r / R_OUT) * 0.18
    if bcurve < 0:    bcurve += 1.0
    if bcurve < 0.55:
        # blade thickness — full at mid-radius, taper toward hub and rim
        t = (r - R_HUB) / (R_IN - R_HUB)     # 0 at hub, 1 at rim
        # bell curve in t (peaks in the middle); flatten the top so most
        # of the blade is at full alpha
        thick = math.sin(t * math.pi)
        if thick > 0.25:
            # boost to near-opaque; keep edges a bit softer for AA
            return min(255, int(255 * (0.55 + thick * 0.65)))
    return 0

# ----- TRUE_COLOR_ALPHA emitter -----
# Each pixel is 4 bytes in LVGL's BGRA8888 layout (LV_COLOR_DEPTH=32).
# This format rotates cleanly via lv_img_set_angle; ALPHA_8BIT doesn't.
def write_bgra(name, pixels, w, h, fout):
    """`pixels` is a list of (b, g, r, a) tuples (w*h items)."""
    fout.write(f"/* {name}: {w}x{h} BGRA8888 (TRUE_COLOR_ALPHA), generated */\n")
    fout.write(f"#include \"lvgl/lvgl.h\"\n\n")
    fout.write(f"static const uint8_t {name}_data[] = {{\n")
    for y in range(h):
        line = "    "
        for x in range(w):
            b, g, r, a = pixels[y*w + x]
            line += f"0x{b:02x},0x{g:02x},0x{r:02x},0x{a:02x},"
        fout.write(line + "\n")
    fout.write("};\n\n")
    fout.write(f"const lv_img_dsc_t {name} = {{\n")
    fout.write(f"    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n")
    fout.write(f"    .header.always_zero = 0,\n")
    fout.write(f"    .header.reserved = 0,\n")
    fout.write(f"    .header.w = {w},\n")
    fout.write(f"    .header.h = {h},\n")
    fout.write(f"    .data_size = sizeof({name}_data),\n")
    fout.write(f"    .data = {name}_data,\n")
    fout.write("};\n")

def gen_bgra(name, w, h, alpha_fn, rgb, outfile):
    """Bake a fixed color into a TRUE_COLOR_ALPHA image driven by alpha_fn."""
    r, g, b = rgb
    pix = []
    for y in range(h):
        for x in range(w):
            a = alpha_fn(x, y)
            pix.append((b, g, r, a) if a > 0 else (0, 0, 0, 0))
    with open(outfile, "w") as f:
        write_bgra(name, pix, w, h, f)

# Bake the fan in cyan so rotation works without runtime recolor.
gen_bgra("icon_fan", W_FAN, H_FAN, fan_alpha, (0x66, 0xdd, 0xff), "icon_fan.h")
print("Wrote icon_flame.h, icon_faucet.h, icon_drop.h, icon_fan.h (BGRA)")

# ----- per-waste-type icons (alpha-8, recolored at runtime) -----
# Newspaper for "Papier" — 44x44 instead of 60x60 so its visual weight
# matches the milk/leaf icons next to it (they're mostly outline, news is
# solid → at the same bounding-box size news looked much bigger).
W_NEWS, H_NEWS = 44, 44
def news_alpha(x, y):
    # Front-page look, scaled down:
    #   - thin outer rectangle (1-px stroke)
    #   - masthead band, headline bar, photo block, text lines
    on_outline = ((x in (3, 40) and 3 <= y <= 40) or
                  (y in (3, 40) and 3 <= x <= 40))
    in_masthead = (5 <= x <= 38 and 5 <= y <= 10)
    in_headline = (5 <= x <= 38 and 13 <= y <= 15)
    in_photo    = (5 <= x <= 16 and 18 <= y <= 27)
    on_right_text = (y in (19, 22, 25) and 18 <= x <= 38)
    on_lower_text = (y in (30, 33, 36) and 5 <= x <= 38)
    if on_outline or in_masthead or in_headline or in_photo:
        return 255
    if on_right_text or on_lower_text:
        return 180
    return 0

# Milk carton for "Plastic" — gable-top carton silhouette.
W_MILK, H_MILK = 60, 60
def milk_alpha(x, y):
    # body: rect (16,20) to (44,54)
    on_body = ((x in (16, 44) and 20 <= y <= 54) or
               (y in (20, 54) and 16 <= x <= 44))
    # gable top: two diagonals from (16,20)/(44,20) up to (30,6)
    # left diagonal: y = 20 - (x-16)*1.0  for x in 16..30
    on_gable_l = (16 <= x <= 30 and y == 20 - (x - 16))
    # right diagonal: y = 20 - (44-x)*1.0  for x in 30..44
    on_gable_r = (30 <= x <= 44 and y == 20 - (44 - x))
    # cap line across top of gable
    on_cap = (28 <= x <= 32 and 4 <= y <= 8)
    if on_body or on_gable_l or on_gable_r or on_cap:
        return 255
    return 0

# Leaf for "GFT" — simple teardrop with a midrib.
W_LEAF, H_LEAF = 60, 60
def leaf_alpha(x, y):
    cx, cy = 30, 30
    # ellipse-ish leaf body, rotated 45° (use untransformed ellipse here)
    dx, dy = x - cx, y - cy
    # rotated coords: u along leaf axis, v perpendicular
    import math
    a = math.radians(-45)
    u =  dx * math.cos(a) + dy * math.sin(a)
    v = -dx * math.sin(a) + dy * math.cos(a)
    # leaf body: u in [-22, 22], v in [-12, 12] tapered at the tips
    half = 12.0 * math.sqrt(max(0.0, 1.0 - (u / 22.0) ** 2))
    on_body = abs(v) <= half and abs(u) <= 22.0
    # midrib: line along u-axis (v in [-1,1])
    on_rib  = abs(v) <= 1.0 and abs(u) <= 22.0
    if on_body or on_rib:
        # soft AA at the rim
        d = abs(v) - half
        if d > -1: return int(max(0.0, min(1.0, (1.0 - d) * 0.5)) * 255)
        return 255
    return 0

gen("icon_news",  W_NEWS, H_NEWS, news_alpha,  "icon_news.h")
gen("icon_milk",  W_MILK, H_MILK, milk_alpha,  "icon_milk.h")
gen("icon_leaf",  W_LEAF, H_LEAF, leaf_alpha,  "icon_leaf.h")
print("Wrote icon_news.h, icon_milk.h, icon_leaf.h")

# Wind arrow — short broad triangle (tip up) with a short shaft below it.
# Emitted as BGRA so lv_img_set_angle can rotate it cleanly per direction.
W_ARR, H_ARR = 32, 32
def arrow_alpha(x, y):
    cx = W_ARR / 2.0 - 0.5
    # Triangular head: tip at y=4, base at y=18, half-width grows linearly.
    if 4 <= y <= 18:
        t = (y - 4) / 14.0
        half = t * 11.0
        if abs(x - cx) <= half: return 255
    # Shaft: 4 px wide, from y=18 to y=27.
    if 18 <= y <= 27 and abs(x - cx) <= 2: return 255
    return 0

gen_bgra("icon_wind_arrow", W_ARR, H_ARR, arrow_alpha,
         (0xff, 0xff, 0xff), "icon_wind_arrow.h")
print("Wrote icon_wind_arrow.h")
