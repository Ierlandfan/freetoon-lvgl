#!/usr/bin/env python3
"""
Generates weather icons matching the buienradar letter codes we care about.
Output: icon_wx_<name>.h (40x40) and icon_wx_<name>_lg.h (80x80).
Each icon is alpha-8bit (recolorable). Drawn procedurally — no assets.
"""
import math, sys

# This file generates two sizes — drawing routines below switch via W/H.
W = H = 40

def write(name, pixels, out):
    out.write(f"#include \"lvgl/lvgl.h\"\n\n")
    out.write(f"static const uint8_t {name}_data[] = {{\n")
    for y in range(H):
        row = pixels[y*W:(y+1)*W]
        out.write("    " + ", ".join(f"0x{p:02x}" for p in row) + ",\n")
    out.write("};\n\n")
    out.write(f"const lv_img_dsc_t {name} = {{\n"
              f"    .header.cf = LV_IMG_CF_ALPHA_8BIT,\n"
              f"    .header.always_zero = 0,\n"
              f"    .header.reserved = 0,\n"
              f"    .header.w = {W},\n    .header.h = {H},\n"
              f"    .data_size = sizeof({name}_data),\n"
              f"    .data = {name}_data,\n}};\n")

def blank():
    return [0] * (W * H)

def setp(px, x, y, a):
    if 0 <= x < W and 0 <= y < H:
        idx = y * W + x
        if a > px[idx]: px[idx] = a   # max-merge so overlapping shapes don't darken

def disc(px, cx, cy, r, a=255, soft=1.0):
    """Filled circle with light AA at the edge."""
    for y in range(int(cy - r - 1), int(cy + r + 2)):
        for x in range(int(cx - r - 1), int(cx + r + 2)):
            d = math.hypot(x - cx, y - cy) - r
            if d <= -soft:
                setp(px, x, y, a)
            elif d < soft:
                t = 1 - (d + soft) / (2 * soft)
                setp(px, x, y, int(a * t))

def ring(px, cx, cy, r, a=255, w=1.2):
    """Stroked circle of width w."""
    for y in range(int(cy - r - w - 1), int(cy + r + w + 2)):
        for x in range(int(cx - r - w - 1), int(cx + r + w + 2)):
            d = abs(math.hypot(x - cx, y - cy) - r)
            if d < w:
                t = 1 - d / w
                setp(px, x, y, int(a * t))

def line(px, x0, y0, x1, y1, a=255, w=1.2):
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(steps + 1):
        t = i / steps if steps else 0
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                dd = math.hypot(dx, dy)
                if dd < w:
                    setp(px, int(x + dx), int(y + dy),
                         int(a * (1 - dd / w)))

# --- Sun ---
def sun():
    p = blank()
    disc(p, 20, 20, 8, 255)
    # rays
    for ang in range(0, 360, 45):
        rad = math.radians(ang)
        x0 = 20 + math.cos(rad) * 11
        y0 = 20 + math.sin(rad) * 11
        x1 = 20 + math.cos(rad) * 17
        y1 = 20 + math.sin(rad) * 17
        line(p, x0, y0, x1, y1, 255, 1.4)
    return p

# --- Cloud (single fluffy cloud) ---
def cloud(p=None, ox=0, oy=0, alpha=255):
    if p is None: p = blank()
    disc(p, 14 + ox, 24 + oy, 7, alpha)
    disc(p, 22 + ox, 20 + oy, 9, alpha)
    disc(p, 30 + ox, 25 + oy, 7, alpha)
    # flat base
    for y in range(28 + oy, 32 + oy):
        for x in range(10 + ox, 36 + ox):
            setp(p, x, y, alpha)
    return p

# --- Sun + small cloud ---
def sun_cloud():
    p = blank()
    # sun upper-right
    disc(p, 26, 14, 5, 200)
    for ang in range(0, 360, 60):
        rad = math.radians(ang)
        line(p, 26 + math.cos(rad)*7, 14 + math.sin(rad)*7,
                26 + math.cos(rad)*11, 14 + math.sin(rad)*11, 200, 1.2)
    cloud(p, ox=-3, oy=4, alpha=255)
    return p

# --- Light rain (cloud with 2 drops) ---
def rain_light():
    p = cloud()
    # drops
    for (dx, dy) in [(13, 36), (22, 38), (30, 36)]:
        # small teardrop
        for j in range(-3, 4):
            for i in range(-2, 3):
                if i*i + j*j*0.4 < 4:
                    setp(p, dx + i, dy + j, 230)
    return p

# --- Heavy rain (cloud with many drops) ---
def rain_heavy():
    p = cloud()
    for (dx, dy) in [(10, 36), (16, 37), (22, 36), (28, 37), (34, 36),
                     (13, 32), (19, 33), (25, 32), (31, 33)]:
        for j in range(-2, 3):
            for i in range(-1, 2):
                if i*i + j*j*0.5 < 3:
                    setp(p, dx + i, dy + j, 230)
    return p

# --- Thunder (cloud with bolt) ---
def thunder():
    p = cloud()
    # zigzag lightning bolt
    pts = [(22, 30), (16, 36), (21, 35), (17, 39)]
    for i in range(len(pts) - 1):
        line(p, pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1], 230, 1.8)
    return p

# --- Bolt only (no cloud) — used as a yellow overlay on top of a
# separately-recolored gray cloud, so the thunder pictogram reads as
# "stormy gray cloud + bright yellow bolt" instead of a monochrome
# amber blob. ---
def bolt():
    p = blank()
    pts = [(22, 16), (16, 24), (22, 22), (16, 32)]
    for i in range(len(pts) - 1):
        line(p, pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1], 255, 2.2)
    return p

# --- Moon (waxing crescent shape — looks night-y without being phase-
# specific. The phase-specific tweak lives in moon_phase() below which
# subtracts a smaller disc from this base to carve out the dark side. )
def moon_full():
    """Full disc — bright moon for full-phase days."""
    p = blank()
    disc(p, 20, 20, 12, 255)
    return p

def moon_phase(phase01):
    """phase01 in [0,1) — 0=new, 0.25=first quarter, 0.5=full, 0.75=last quarter.
       Returns a 40x40 alpha pixel array of the moon at that phase, rendered
       as a bright disc with a dark disc subtracted to carve out the shadowed
       side. We approximate the terminator as another circle with an x-offset
       that depends on the phase angle."""
    p = blank()
    cx, cy, r = 20, 20, 12
    disc(p, cx, cy, r, 255)
    # phase angle (0 to 2π); the shadow disc's offset from the moon's center
    # is proportional to cos(phase angle), and its sign flips between waxing
    # and waning halves of the lunar month.
    ang = phase01 * 2 * math.pi
    # Use a larger shadow disc so it cuts a clean concave edge.
    shadow_r = int(r * 1.2)
    # For phase01 ∈ (0, 0.5) the shadow rides the LEFT side and shrinks
    # toward 0 at full. For phase01 ∈ (0.5, 1) it rides the RIGHT side
    # and grows back toward new.
    offset = int(math.cos(ang) * (r + 2))
    # subtract the shadow disc — write 0 (transparent) inside it
    for y in range(cy - shadow_r - 1, cy + shadow_r + 2):
        for x in range(cx + offset - shadow_r - 1, cx + offset + shadow_r + 2):
            if 0 <= x < W and 0 <= y < H:
                d = math.hypot(x - (cx + offset), y - cy)
                if d <= shadow_r:
                    p[y * W + x] = 0
    return p

# A single "moon" icon stored in the catalog — pick approximate-half phase
# (waxing gibbous) as the default. Runtime code picks the phase variant
# below per-tile based on current date.
def moon():
    return moon_phase(0.30)

# --- Snow (cloud with stars) ---
def snow():
    p = cloud()
    for (sx, sy) in [(12, 36), (20, 38), (28, 36), (16, 33), (24, 33)]:
        # tiny 6-point cross
        line(p, sx-2, sy, sx+2, sy, 230, 1.0)
        line(p, sx, sy-2, sx, sy+2, 230, 1.0)
        line(p, sx-1, sy-1, sx+1, sy+1, 230, 1.0)
        line(p, sx-1, sy+1, sx+1, sy-1, 230, 1.0)
    return p

# --- Fog (horizontal bands) ---
def fog():
    p = blank()
    for (y, span) in [(13, (8, 32)), (18, (6, 34)), (23, (10, 30)),
                      (28, (4, 36)), (33, (8, 28))]:
        for x in range(span[0], span[1]):
            for dy in (-1, 0, 1):
                setp(p, x, y + dy, 200 if dy == 0 else 100)
    return p

# --- Overcast / fully cloudy (default fallback) ---
def overcast():
    p = cloud(alpha=255)
    cloud(p, ox=4, oy=-2, alpha=180)
    return p

def emit_all(size_px, suffix):
    global W, H
    W = H = size_px
    icons = [
        ("icon_wx_sun"       + suffix, sun()),
        ("icon_wx_sun_cloud" + suffix, sun_cloud()),
        ("icon_wx_cloud"     + suffix, overcast()),
        ("icon_wx_rain_light"+ suffix, rain_light()),
        ("icon_wx_rain_heavy"+ suffix, rain_heavy()),
        ("icon_wx_thunder"   + suffix, thunder()),
        ("icon_wx_bolt"      + suffix, bolt()),
        ("icon_wx_moon"      + suffix, moon()),
        ("icon_wx_snow"      + suffix, snow()),
        ("icon_wx_fog"       + suffix, fog()),
    ]
    # Phase-specific moons — 8 named slots covering the lunar month. The
    # runtime picks the nearest one from the current date. New (0.0) is a
    # purely-blank disc which would render as nothing, so we draw it as a
    # very thin crescent — slightly visible — so the slot doesn't disappear
    # on a new-moon night. Other phases follow moon_phase() exactly.
    moon_phases = [
        ("icon_wx_moon_new"      + suffix, moon_phase(0.04)),
        ("icon_wx_moon_wax_cres" + suffix, moon_phase(0.125)),
        ("icon_wx_moon_first_q"  + suffix, moon_phase(0.25)),
        ("icon_wx_moon_wax_gib"  + suffix, moon_phase(0.375)),
        ("icon_wx_moon_full"     + suffix, moon_full()),
        ("icon_wx_moon_wan_gib"  + suffix, moon_phase(0.625)),
        ("icon_wx_moon_last_q"   + suffix, moon_phase(0.75)),
        ("icon_wx_moon_wan_cres" + suffix, moon_phase(0.875)),
    ]
    icons += moon_phases
    for name, px in icons:
        with open(f"{name}.h", "w") as f:
            write(name, px, f)
    print(f"  wrote {len(icons)} icons at {size_px}x{size_px}")

# 40x40 — home forecast band columns.
emit_all(40, "")

# 80x80 — forecast detail strip + dim screen. Same procedural drawing
# scales naturally because pixel coords use 0..W/H references.
# But our procedural code is hard-coded to 40px coords. Re-scale by 2x:
# patch the drawing helpers' constants temporarily.

# Easiest: just procedurally rerun with W=H=80 — but our shapes use
# absolute pixel positions tuned for 40x40. Let me 2x-scale all the
# magic numbers inside sun/cloud/etc functions.

def disc2(px, cx, cy, r, a=255, soft=2.0):
    for y in range(int(cy - r - 2), int(cy + r + 3)):
        for x in range(int(cx - r - 2), int(cx + r + 3)):
            d = math.hypot(x - cx, y - cy) - r
            if d <= -soft:
                setp(px, x, y, a)
            elif d < soft:
                t = 1 - (d + soft) / (2 * soft)
                setp(px, x, y, int(a * t))

def line2(px, x0, y0, x1, y1, a=255, w=2.4):
    steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(steps + 1):
        t = i / steps if steps else 0
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                dd = math.hypot(dx, dy)
                if dd < w:
                    setp(px, int(x + dx), int(y + dy),
                         int(a * (1 - dd / w)))

# Big variants — 2x of the 40px versions.
def sun_lg():
    p = blank()
    disc2(p, 40, 40, 16, 255)
    for ang in range(0, 360, 45):
        rad = math.radians(ang)
        line2(p, 40 + math.cos(rad)*22, 40 + math.sin(rad)*22,
                 40 + math.cos(rad)*34, 40 + math.sin(rad)*34, 255, 2.6)
    return p

def cloud_lg(p=None, ox=0, oy=0, alpha=255):
    if p is None: p = blank()
    disc2(p, 28 + ox, 48 + oy, 14, alpha)
    disc2(p, 44 + ox, 40 + oy, 18, alpha)
    disc2(p, 60 + ox, 50 + oy, 14, alpha)
    for y in range(56 + oy, 64 + oy):
        for x in range(20 + ox, 72 + ox):
            setp(p, x, y, alpha)
    return p

def sun_cloud_lg():
    p = blank()
    disc2(p, 52, 28, 10, 200)
    for ang in range(0, 360, 60):
        rad = math.radians(ang)
        line2(p, 52 + math.cos(rad)*14, 28 + math.sin(rad)*14,
                 52 + math.cos(rad)*22, 28 + math.sin(rad)*22, 200, 2.4)
    cloud_lg(p, ox=-6, oy=8, alpha=255)
    return p

def rain_light_lg():
    p = cloud_lg()
    for (dx, dy) in [(26, 72), (44, 76), (60, 72)]:
        for j in range(-6, 8):
            for i in range(-4, 6):
                if i*i + j*j*0.4 < 16:
                    setp(p, dx + i, dy + j, 230)
    return p

def rain_heavy_lg():
    p = cloud_lg()
    for (dx, dy) in [(20, 72), (32, 74), (44, 72), (56, 74), (68, 72),
                     (26, 64), (38, 66), (50, 64), (62, 66)]:
        for j in range(-4, 6):
            for i in range(-2, 4):
                if i*i + j*j*0.5 < 12:
                    setp(p, dx + i, dy + j, 230)
    return p

def thunder_lg():
    p = cloud_lg()
    pts = [(44, 60), (32, 72), (42, 70), (34, 78)]
    for i in range(len(pts) - 1):
        line2(p, pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1], 230, 3.6)
    return p

def bolt_lg():
    p = blank()
    pts = [(44, 32), (32, 48), (44, 44), (32, 64)]
    for i in range(len(pts) - 1):
        line2(p, pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1], 255, 4.4)
    return p

def moon_phase_lg(phase01):
    """80x80 phase-aware moon. Same algorithm as moon_phase()."""
    p = blank()
    cx, cy, r = 40, 40, 24
    disc2(p, cx, cy, r, 255)
    shadow_r = int(r * 1.2)
    ang = phase01 * 2 * math.pi
    offset = int(math.cos(ang) * (r + 4))
    for y in range(cy - shadow_r - 2, cy + shadow_r + 3):
        for x in range(cx + offset - shadow_r - 2, cx + offset + shadow_r + 3):
            if 0 <= x < W and 0 <= y < H:
                d = math.hypot(x - (cx + offset), y - cy)
                if d <= shadow_r:
                    p[y * W + x] = 0
    return p

def moon_full_lg():
    p = blank()
    disc2(p, 40, 40, 24, 255)
    return p

def moon_lg():
    return moon_phase_lg(0.30)

def snow_lg():
    p = cloud_lg()
    for (sx, sy) in [(24, 72), (40, 76), (56, 72), (32, 66), (48, 66)]:
        line2(p, sx-4, sy, sx+4, sy, 230, 2.0)
        line2(p, sx, sy-4, sx, sy+4, 230, 2.0)
        line2(p, sx-2, sy-2, sx+2, sy+2, 230, 2.0)
        line2(p, sx-2, sy+2, sx+2, sy-2, 230, 2.0)
    return p

def fog_lg():
    p = blank()
    for (y, span) in [(26, (16, 64)), (36, (12, 68)), (46, (20, 60)),
                      (56, (8, 72)), (66, (16, 56))]:
        for x in range(span[0], span[1]):
            for dy in (-2, -1, 0, 1, 2):
                setp(p, x, y + dy, 200 if abs(dy) <= 1 else 100)
    return p

def overcast_lg():
    p = cloud_lg(alpha=255)
    cloud_lg(p, ox=8, oy=-4, alpha=180)
    return p

W = H = 80
icons_lg = [
    ("icon_wx_sun_lg",        sun_lg()),
    ("icon_wx_sun_cloud_lg",  sun_cloud_lg()),
    ("icon_wx_cloud_lg",      overcast_lg()),
    ("icon_wx_rain_light_lg", rain_light_lg()),
    ("icon_wx_rain_heavy_lg", rain_heavy_lg()),
    ("icon_wx_thunder_lg",    thunder_lg()),
    ("icon_wx_bolt_lg",       bolt_lg()),
    ("icon_wx_moon_lg",       moon_lg()),
    ("icon_wx_moon_new_lg",      moon_phase_lg(0.04)),
    ("icon_wx_moon_wax_cres_lg", moon_phase_lg(0.125)),
    ("icon_wx_moon_first_q_lg",  moon_phase_lg(0.25)),
    ("icon_wx_moon_wax_gib_lg",  moon_phase_lg(0.375)),
    ("icon_wx_moon_full_lg",     moon_full_lg()),
    ("icon_wx_moon_wan_gib_lg",  moon_phase_lg(0.625)),
    ("icon_wx_moon_last_q_lg",   moon_phase_lg(0.75)),
    ("icon_wx_moon_wan_cres_lg", moon_phase_lg(0.875)),
    ("icon_wx_snow_lg",       snow_lg()),
    ("icon_wx_fog_lg",        fog_lg()),
]
for name, px in icons_lg:
    with open(f"{name}.h", "w") as f:
        write(name, px, f)
print(f"  wrote {len(icons_lg)} icons at 80x80")

# Also: a trash-can icon for waste alerts.
def trash_lg():
    p = blank()
    # lid handle
    for y in range(8, 14):
        for x in range(32, 48):
            setp(p, x, y, 255)
    # lid bar
    for y in range(14, 20):
        for x in range(16, 64):
            setp(p, x, y, 255)
    # bin body — slightly tapered
    for y in range(20, 72):
        t = (y - 20) / 52.0
        x0 = int(22 + t * 2)
        x1 = int(58 - t * 2)
        for x in range(x0, x1):
            setp(p, x, y, 255)
    # three vertical stripes (cut-outs)
    for y in range(26, 66):
        for s in (30, 39, 48):
            setp(p, s,   y, 0)
            setp(p, s+1, y, 0)
    return p

W = H = 80
with open("icon_trash_lg.h", "w") as f:
    write("icon_trash_lg", trash_lg(), f)
print("  wrote 1 trash icon at 80x80")

# Smaller trash for the home Waste tile (alpha-8bit doesn't recolor under
# transform, so use a native-size 40x40 source).
def trash_sm():
    p = blank()
    # handle
    for y in range(4, 7):
        for x in range(16, 24):
            setp(p, x, y, 255)
    # lid
    for y in range(7, 10):
        for x in range(8, 32):
            setp(p, x, y, 255)
    # bin tapered
    for y in range(10, 36):
        t = (y - 10) / 26.0
        x0 = int(11 + t * 1)
        x1 = int(29 - t * 1)
        for x in range(x0, x1):
            setp(p, x, y, 255)
    # 3 cut-out stripes
    for y in range(13, 33):
        for s in (15, 20, 25):
            setp(p, s, y, 0)
    return p

W = H = 40
with open("icon_trash.h", "w") as f:
    write("icon_trash", trash_sm(), f)
print("  wrote 1 trash icon at 40x40")
