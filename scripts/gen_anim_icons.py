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
        f.write(f"const lv_img_dsc_t {name} = {{\n")
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
# 40 x 58 px
W_B, H_B = 40, 58
def balloon_inside(px, py):
    cx = (W_B - 1) / 2.0  # 19.5
    # Region 1: String (y: 43 .. 57)
    if py >= 43:
        str_x = cx + 2.4 * math.sin((py - 43.0) * 0.45)
        dist = abs(px - str_x)
        return 1.0 if dist <= 0.9 else 0.0

    # Region 2: Knot (y: 39 .. 43)
    if py >= 39:
        t = (py - 39.0) / 4.0
        kw = 3.0 + 3.0 * math.sin(t * math.pi)
        return 1.0 if abs(px - cx) <= kw else 0.0

    # Region 3: Balloon body (y: 0 .. 39)
    # Top dome (y: 0 .. 18)
    cy_dome = 16.5
    r_dome = 15.5
    if py <= cy_dome:
        dx = px - cx
        dy = py - cy_dome
        return 1.0 if (dx*dx + dy*dy) <= r_dome*r_dome else 0.0

    # Lower tapering body (y: 16.5 .. 39)
    t = (py - cy_dome) / (39.0 - cy_dome)  # 0 .. 1
    # Smooth taper from r_dome down to 4.0
    hw = r_dome * (1.0 - 0.72 * math.pow(t, 1.35))
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
# 36 x 36 px
W_H, H_H = 36, 36
def heart_inside(px, py):
    cx = (W_H - 1) / 2.0  # 17.5
    cy = 15.0

    # Coordinate centered at heart
    # Left lobe center: (-7.0, -3.0), Right lobe center: (+7.0, -3.0), Radius = 8.0
    r_lobe = 7.8
    dx_l = px - (cx - 7.0)
    dy_l = py - (cy - 3.5)
    if (dx_l*dx_l + dy_l*dy_l) <= r_lobe*r_lobe:
        return 1.0

    dx_r = px - (cx + 7.0)
    dy_r = py - (cy - 3.5)
    if (dx_r*dx_r + dy_r*dy_r) <= r_lobe*r_lobe:
        return 1.0

    # Bottom triangular wedge meeting at (cx, cy + 16.0)
    # Left edge: from (cx - 14.8, cy - 3.5) to (cx, cy + 16.0)
    # Right edge: from (cx + 14.8, cy - 3.5) to (cx, cy + 16.0)
    tip_y = cy + 16.0
    if py >= (cy - 3.5) and py <= tip_y:
        t = (tip_y - py) / (tip_y - (cy - 3.5))  # 0 at tip, 1 at lobes
        hw = 14.8 * t
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

if __name__ == '__main__':
    b_data = gen_balloon()
    write_header("icon_anim_balloon", b_data, W_B, H_B, "lvgl_ui_recovered/src/icon_anim_balloon.h")

    h_data = gen_heart()
    write_header("icon_anim_heart", h_data, W_H, H_H, "lvgl_ui_recovered/src/icon_anim_heart.h")

