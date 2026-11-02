#!/usr/bin/env python3
"""
Generate icon_anim_balloon.h and icon_anim_heart.h as LVGL ALPHA_8BIT descriptors.
"""
import math

def write_header(name, arr, w, h, filepath):
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

# --- AUTUMN LEAF GENERATOR ---
# Broad full-face oak/maple leaf: 64 x 64 px (body ~52x55 px)
W_L, H_L = 64, 64
def leaf_inside(px, py):
    cx = 31.5
    # Curved woody stem: y from 48 to 60
    if py >= 48:
        t = (py - 48.0) / 12.0
        sx = cx - 5.0 * math.sin(t * 0.9)
        return 1.0 if abs(px - sx) <= 1.5 and py <= 60 else 0.0

    if py < 5 or py > 48:
        return 0.0

    t = (py - 5.0) / 43.0  # 0.0 top tip, 1.0 base
    midrib_x = cx + 2.0 * math.sin(t * math.pi)

    # Broad multi-lobed silhouette (envelope + deep oak lobes)
    env = 24.0 * math.sin(math.pow(t, 0.7) * math.pi)
    lobes = 7.0 * math.sin(t * math.pi * 4.0 - 0.3)
    hw = env + lobes
    if hw < 2.0: hw = 2.0

    dx = px - midrib_x
    return 1.0 if abs(dx) <= hw else 0.0

def gen_leaf():
    SS = 4
    arr = []
    for y in range(H_L):
        for x in range(W_L):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    sub_x = x + (sx + 0.5) / SS
                    sub_y = y + (sy + 0.5) / SS
                    if leaf_inside(sub_x, sub_y):
                        hits += 1
            alpha = int(round((hits / (SS * SS)) * 255.0))
            arr.append(alpha)
    return arr

if __name__ == '__main__':
    b_data = gen_balloon()
    write_header("icon_anim_balloon", b_data, W_B, H_B, "lvgl_ui_recovered/src/icon_anim_balloon.h")

    h_data = gen_heart()
    write_header("icon_anim_heart", h_data, W_H, H_H, "lvgl_ui_recovered/src/icon_anim_heart.h")

    l_data = gen_leaf()
    write_header("icon_anim_leaf", l_data, W_L, H_L, "lvgl_ui_recovered/src/icon_anim_leaf.h")

