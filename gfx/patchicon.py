import struct, sys

# ndstool layout (banner.h / banner.cpp / crc.h):
#   0x000 u16 version
#   0x002 u16 crc16 (CalcCrc16 over [0x020 : 0x840))
#   0x004..0x020 reserved (28 bytes)
#   0x020 tile_data tile[4][4][8][4]   (32x32 4bpp, 8x8 tiled, left px = low nibble)
#   0x220 palette 16 x u16 RGB555
#   0x240 title[6][128]

def crc16_ref(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF

def bmp_grid(bmp_path):
    d = open(bmp_path, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    off = struct.unpack_from('<I', d, 10)[0]
    assert w == 32 and h == 32, "icon must be 32x32"
    rowbytes = ((w * bpp + 31) // 32) * 4
    data = d[off:]
    g = [[0] * 32 for _ in range(32)]
    for iy in range(32):                 # bottom-up BMP -> top-down grid
        row = data[(31 - iy) * rowbytes:(32 - iy) * rowbytes]
        for ix in range(32):
            b = row[ix // 2]
            g[iy][ix] = ((b >> 4) & 0xF) if (ix & 1) == 0 else (b & 0xF)
    pal = [tuple(d[54 + i * 4:54 + i * 4 + 3]) for i in range(16)]  # B,G,R
    return g, pal

def tiled_icon(grid):
    out = bytearray(512)
    k = 0
    for tr in range(4):
        for tc in range(4):
            for y in range(8):
                for x in range(0, 8, 2):
                    left = grid[tr * 8 + y][tc * 8 + x]
                    right = grid[tr * 8 + y][tc * 8 + x + 1]
                    out[k] = (right << 4) | left
                    k += 1
    return bytes(out)

def palette_bytes(pal):
    out = bytearray()
    for c in pal:                        # c = (B, G, R)
        out += struct.pack('<H', ((c[2] >> 3) & 0x1F) | (((c[1] >> 3) & 0x1F) << 5) | (((c[0] >> 3) & 0x1F) << 10))
    return bytes(out)

def patch(nds, bmp, out):
    d = bytearray(open(nds, 'rb').read())
    B = struct.unpack_from('<I', d, 0x68)[0]
    assert B + 0x840 <= len(d), "banner out of file"
    grid, pal = bmp_grid(bmp)
    icon, palb = tiled_icon(grid), palette_bytes(pal)
    d[B:B + 2] = struct.pack('<H', 1)                      # version
    d[B + 4:B + 0x20] = bytes(28)                          # reserved = 0
    d[B + 0x20:B + 0x220] = icon
    d[B + 0x220:B + 0x240] = palb
    crc = crc16_ref(bytes(d[B + 0x20:B + 0x840]))
    d[B + 2:B + 4] = struct.pack('<H', crc)
    open(out, 'wb').write(bytes(d))
    print("patched banner at", hex(B), "crc", hex(crc), "pal[0]", hex(struct.unpack_from('<H', palb, 0)[0]))
    return icon, palb

if __name__ == '__main__':
    patch(sys.argv[1], sys.argv[2], sys.argv[3])