import struct, sys

def convert(ppm_path, out_path):
    with open(ppm_path, "rb") as f:
        magic = f.readline().strip()
        line = f.readline().strip()
        while line.startswith(b'#'):
            line = f.readline().strip()
        dims = line.split()
        w, h = int(dims[0]), int(dims[1])
        maxval = f.readline().strip()
        data = f.read()

    row_padded = (w * 3 + 3) & ~3
    pad = b'\x00' * (row_padded - w * 3)
    bmp_data = bytearray()
    for y in range(h - 1, -1, -1):
        row = data[y * w * 3:(y + 1) * w * 3]
        bgr = bytearray(w * 3)
        bgr[0::3] = row[2::3]
        bgr[1::3] = row[1::3]
        bgr[2::3] = row[0::3]
        bmp_data.extend(bgr)
        bmp_data.extend(pad)

    file_size = 54 + len(bmp_data)
    hdr = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 54)
    info = struct.pack('<IIIHHIIIIII', 40, w, h, 1, 24, 0, len(bmp_data), 2835, 2835, 0, 0)
    with open(out_path, "wb") as f:
        f.write(hdr + info + bmp_data)
    print(f"Wrote {out_path} ({w}x{h})")

if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2])

