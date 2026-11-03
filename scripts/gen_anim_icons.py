#!/usr/bin/env python3
"""
Generate icon_anim_balloon.h and icon_anim_heart.h as LVGL ALPHA_8BIT descriptors.
"""
import math
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(SCRIPT_DIR, "..", "lvgl_ui_recovered", "src")

def write_header(name, arr, w, h, filename):
    filepath = os.path.join(SRC_DIR, filename)
    with open(filepath, "w") as f:
        f.write(f"/* {name}: {w}x{h} alpha-8bit, generated, do not edit */\n")
        f.write('#include "lvgl/lvgl.h"\n\n')
        f.write(f"static const uint8_t {name}_data[] = {{\n")
        for y in range(h):
            row = arr[y*w : (y+1)*w]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in row) + ",\n")
        f.write("};\n\n")
        f.write(f"static const lv_img_dsc_t {name} = {{\n")
        f.write(f"    .header.cf = LV_IMG_CF_ALPHA_8BIT,\n")
        f.write(f"    .header.always_zero = 0,\n")
        f.write(f"    .header.reserved = 0,\n")
        f.write(f"    .header.w = {w},\n")
        f.write(f"    .header.h = {h},\n")
        f.write(f"    .data_size = sizeof({name}_data),\n")
        f.write(f"    .data = {name}_data,\n")
        f.write("};\n")
    print(f"Wrote {filepath} ({w}x{h})")

# --- BALLOON GENERATOR ---
# Restored full size: 54 x 84 px (body ~44x56, knot ~6x6, wavy string ~22px)
W_B, H_B = 54, 84
def balloon_inside(px, py):
    cx = (W_B - 1) / 2.0  # 26.5
    # Region 1: String (y: 62 .. 83)
    if py >= 62:
        str_x = cx + 3.2 * math.sin((py - 62.0) * 0.40)
        dist = abs(px - str_x)
        return 1.0 if dist <= 1.0 else 0.0

    # Region 2: Knot (y: 56 .. 62)
    if py >= 56:
        t = (py - 56.0) / 6.0
        kw = 4.0 + 3.5 * math.sin(t * math.pi)
        return 1.0 if abs(px - cx) <= kw else 0.0

    # Region 3: Balloon body (y: 0 .. 56)
    cy_dome = 24.0
    r_dome = 22.0
    if py <= cy_dome:
        dx = px - cx
        dy = py - cy_dome
        return 1.0 if (dx*dx + dy*dy) <= r_dome*r_dome else 0.0

    # Lower tapering body (y: 24.0 .. 56)
    t = (py - cy_dome) / (56.0 - cy_dome)  # 0 .. 1
    hw = r_dome * (1.0 - 0.75 * math.pow(t, 1.35))
    return 1.0 if abs(px - cx) <= hw else 0.0

def gen_balloon():
    SS = 4
    arr = []
    for y in range(H_B):
        for x in range(W_B):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    sub_x = x + (sx + 0.5) / SS
                    sub_y = y + (sy + 0.5) / SS
                    if balloon_inside(sub_x, sub_y):
                        hits += 1
            alpha = int(round((hits / (SS * SS)) * 255.0))
            arr.append(alpha)
    return arr

# --- HEART GENERATOR ---
# Full bold size: 64 x 64 px (body ~55x50 px)
W_H, H_H = 64, 64
def heart_inside(px, py):
    cx = 31.5
    cy = 21.0
    r_lobe = 14.0
    # Left lobe
    dx_l = px - (cx - 12.5)
    dy_l = py - cy
    if (dx_l*dx_l + dy_l*dy_l) <= r_lobe*r_lobe:
        return 1.0
    # Right lobe
    dx_r = px - (cx + 12.5)
    dy_r = py - cy
    if (dx_r*dx_r + dy_r*dy_r) <= r_lobe*r_lobe:
        return 1.0
    # Bottom wedge
    tip_y = 56.0
    if py >= cy and py <= tip_y:
        t = (tip_y - py) / (tip_y - cy)
        hw = 25.5 * math.pow(t, 0.95)
        if abs(px - cx) <= hw:
            return 1.0
    return 0.0

def gen_heart():
    SS = 4
    arr = []
    for y in range(H_H):
        for x in range(W_H):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    sub_x = x + (sx + 0.5) / SS
                    sub_y = y + (sy + 0.5) / SS
                    if heart_inside(sub_x, sub_y):
                        hits += 1
            alpha = int(round((hits / (SS * SS)) * 255.0))
            arr.append(alpha)
    return arr

# --- AUTUMN MAPLE LEAF GENERATOR ---
# Iconic Canadian / Sugar Maple leaf with 5 lobes, sharp points and palmate venation: 64 x 64 px
W_L, H_L = 64, 64
def gen_leaf():
    from PIL import Image, ImageDraw
    scale = 8
    sz = W_L * scale
    im = Image.new('L', (sz, sz), 0)
    draw = ImageDraw.Draw(im)

    # Highly refined Canadian / Sugar Maple (Acer saccharum) profile
    right_pts = [
        (32.0, 3.5),    # Central apex
        (34.2, 9.5),    # Central upper notch
        (39.0, 11.5),   # Central side tooth
        (37.5, 17.0),   # Central lower contour
        (35.5, 23.5),   # Deep primary sinus (U-notch between central & lateral)
        (43.5, 21.0),   # Lateral upper tooth
        (44.0, 24.5),   # Lateral upper notch
        (58.5, 25.0),   # Lateral main apex tip
        (51.0, 31.0),   # Lateral lower notch
        (53.5, 34.5),   # Lateral lower tooth
        (42.0, 37.5),   # Deep secondary sinus
        (48.0, 41.5),   # Basal tooth
        (44.0, 44.5),   # Basal notch
        (45.5, 48.0),   # Basal lower tip
        (38.0, 48.5),   # Base inward curve
        (33.5, 47.0),   # Petiole junction
    ]

    left_pts = [(64.0 - x, y) for (x, y) in reversed(right_pts[1:])]
    all_pts = [(x * scale, y * scale) for (x, y) in (right_pts + left_pts)]

    draw.polygon(all_pts, fill=255)

    # Curved woody stem / petiole
    stem_pts = [
        (31.2 * scale, 46.5 * scale),
        (32.8 * scale, 46.5 * scale),
        (32.2 * scale, 53.5 * scale),
        (29.5 * scale, 61.0 * scale),
        (27.8 * scale, 60.5 * scale),
        (30.6 * scale, 53.5 * scale),
    ]
    draw.polygon(stem_pts, fill=255)

    # Palmate venation: delicate carved veins radiating from base
    base_xy = (32.0 * scale, 47.0 * scale)
    vein_pts = [
        (32.0 * scale, 7.5 * scale),   # to central apex
        (54.0 * scale, 26.0 * scale),  # to right lateral apex
        (10.0 * scale, 26.0 * scale),  # to left lateral apex
        (45.0 * scale, 46.0 * scale),  # to right basal
        (19.0 * scale, 46.0 * scale),  # to left basal
    ]
    for vp in vein_pts:
        draw.line([base_xy, vp], fill=185, width=int(1.2 * scale))

    # Side branches
    draw.line([(32.0 * scale, 24.0 * scale), (37.5 * scale, 13.0 * scale)], fill=195, width=int(0.9 * scale))
    draw.line([(32.0 * scale, 24.0 * scale), (26.5 * scale, 13.0 * scale)], fill=195, width=int(0.9 * scale))
    draw.line([(42.0 * scale, 37.0 * scale), (44.0 * scale, 23.0 * scale)], fill=195, width=int(0.9 * scale))
    draw.line([(22.0 * scale, 37.0 * scale), (20.0 * scale, 23.0 * scale)], fill=195, width=int(0.9 * scale))

    im_res = im.resize((W_L, H_L), Image.Resampling.LANCZOS)
    return list(im_res.getdata())

# --- SNOWFLAKE GENERATOR ---
# Intricate 6-fold hexagonal crystalline snowflake: 64 x 64 px
W_S, H_S = 64, 64
def gen_snowflake():
    from PIL import Image, ImageDraw
    scale = 4
    sz = W_S * scale
    im = Image.new('L', (sz, sz), 0)
    draw = ImageDraw.Draw(im)
    cx, cy = sz // 2, sz // 2
    r_outer = 27.5 * scale

    for i in range(6):
        angle = i * (math.pi / 3.0)
        cos_a = math.cos(angle)
        sin_a = math.sin(angle)

        # Main stem
        x2 = cx + r_outer * cos_a
        y2 = cy + r_outer * sin_a
        draw.line([(cx, cy), (x2, y2)], fill=255, width=int(2.8 * scale))

        # Diamond tip
        tip_len = 3.2 * scale
        draw.polygon([
            (x2 + tip_len * cos_a, y2 + tip_len * sin_a),
            (x2 - tip_len * sin_a, y2 + tip_len * cos_a),
            (x2 - tip_len * 0.5 * cos_a, y2 - tip_len * 0.5 * sin_a),
            (x2 + tip_len * sin_a, y2 - tip_len * cos_a),
        ], fill=255)

        # Inner V branches (at 42% of stem)
        r_b1 = r_outer * 0.42
        bx1 = cx + r_b1 * cos_a
        by1 = cy + r_b1 * sin_a
        b1_len = 9.0 * scale
        for sign in [-1, 1]:
            branch_angle = angle + sign * (math.pi / 3.0)
            draw.line([(bx1, by1), (bx1 + b1_len * math.cos(branch_angle), by1 + b1_len * math.sin(branch_angle))],
                      fill=255, width=int(2.0 * scale))

        # Outer V branches (at 72% of stem)
        r_b2 = r_outer * 0.72
        bx2 = cx + r_b2 * cos_a
        by2 = cy + r_b2 * sin_a
        b2_len = 6.5 * scale
        for sign in [-1, 1]:
            branch_angle = angle + sign * (math.pi / 3.0)
            draw.line([(bx2, by2), (bx2 + b2_len * math.cos(branch_angle), by2 + b2_len * math.sin(branch_angle))],
                      fill=255, width=int(2.0 * scale))

    # Central hexagon nucleus
    hex_pts = []
    for i in range(6):
        ha = i * (math.pi / 3.0) + (math.pi / 6.0)
        hex_pts.append((cx + 5.2 * scale * math.cos(ha), cy + 5.2 * scale * math.sin(ha)))
    draw.polygon(hex_pts, fill=255)

    im_res = im.resize((W_S, H_S), Image.Resampling.LANCZOS)
    return list(im_res.getdata())

# --- PRESENT / GIFT GENERATOR (SINTERKLAAS & KERST) ---
# Festive wrapped gift box with lid, ribbon and bow: 64 x 64 px
W_P, H_P = 64, 64
def gen_present():
    from PIL import Image, ImageDraw
    scale = 4
    sz = W_P * scale
    im = Image.new('L', (sz, sz), 0)
    draw = ImageDraw.Draw(im)

    # Box dimensions
    bx0, by0 = 12 * scale, 24 * scale
    bx1, by1 = 52 * scale, 56 * scale

    # Lid dimensions (slightly wider)
    lx0, ly0 = 9 * scale, 17 * scale
    lx1, ly1 = 55 * scale, 25 * scale

    # Draw box body and lid
    draw.rounded_rectangle([bx0, by0, bx1, by1], radius=3*scale, fill=220)
    draw.rounded_rectangle([lx0, ly0, lx1, ly1], radius=3*scale, fill=255)

    # Bow loops on top of lid
    cx = 32 * scale
    # Left bow loop
    draw.ellipse([cx - 15*scale, ly0 - 11*scale, cx - 1*scale, ly0 + 2*scale], fill=255)
    draw.ellipse([cx - 12*scale, ly0 - 8*scale, cx - 4*scale, ly0], fill=0) # hole

    # Right bow loop
    draw.ellipse([cx + 1*scale, ly0 - 11*scale, cx + 15*scale, ly0 + 2*scale], fill=255)
    draw.ellipse([cx + 4*scale, ly0 - 8*scale, cx + 12*scale, ly0], fill=0) # hole

    # Knot in center
    draw.ellipse([cx - 4.5*scale, ly0 - 4.5*scale, cx + 4.5*scale, ly0 + 4.5*scale], fill=255)

    # Vertical ribbon through box
    rw = 7 * scale
    draw.rectangle([cx - rw//2, ly0, cx + rw//2, by1], fill=255)

    im_res = im.resize((W_P, H_P), Image.Resampling.LANCZOS)
    return list(im_res.getdata())

if __name__ == '__main__':
    b_data = gen_balloon()
    write_header("icon_anim_balloon", b_data, W_B, H_B, "icon_anim_balloon.h")

    h_data = gen_heart()
    write_header("icon_anim_heart", h_data, W_H, H_H, "icon_anim_heart.h")

    l_data = gen_leaf()
    write_header("icon_anim_leaf", l_data, W_L, H_L, "icon_anim_leaf.h")

    s_data = gen_snowflake()
    write_header("icon_anim_snowflake", s_data, W_S, H_S, "icon_anim_snowflake.h")

    p_data = gen_present()
    write_header("icon_anim_present", p_data, W_P, H_P, "icon_anim_present.h")



