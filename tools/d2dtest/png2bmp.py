#!/usr/bin/env python3
"""Convert an 8-bit RGB or RGBA PNG to a 24-bit BMP (what d2dtest reads).

    tools/d2dtest/png2bmp.py IN.png OUT.bmp
"""
import struct, sys, zlib


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, w = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and not interlace, 'needs 8-bit RGB(A), not interlaced'
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def write_bmp(path, w, h, bpp, rows):
    row = (w * 3 + 3) & ~3
    out = bytearray(struct.pack('<2sIHHI', b'BM', 54 + row * h, 0, 0, 54))
    out += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, row * h, 2835, 2835, 0, 0)
    for line in reversed(rows):
        px = bytearray()
        for x in range(w):
            r, g, b = line[x * bpp:x * bpp + 3]
            px += bytes((b, g, r))
        out += px + b'\0' * (row - w * 3)
    open(path, 'wb').write(out)


if __name__ == '__main__':
    write_bmp(sys.argv[2], *read_png(sys.argv[1]))
