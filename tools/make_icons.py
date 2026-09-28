#!/usr/bin/env python3
"""Draw NovaOS's sample icons (Windows .ico files) and sample picture.

    tools/make_icons.py            (rewrites the files listed in ICONS)

Each icon holds a 256x256 PNG image (as Windows Vista+ icons do) and
48, 32, 24 and 16 pixel 32-bit BMP images with their AND masks, every size
drawn natively from signed distance functions (1-pixel anti-aliasing), so
the files exercise both image formats of the kernel's icon decoder.
"""
import math, os, struct, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (256, 48, 32, 24, 16)

def cov(sd):                       # signed distance (px, <0 inside) -> coverage
    return max(0.0, min(1.0, 0.5 - sd))

def rrect(px, py, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    qx = abs(px - cx) - ((x1 - x0) / 2 - r)
    qy = abs(py - cy) - ((y1 - y0) / 2 - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

def over(dst, src, a):             # straight-alpha "over" of an RGB colour
    if a <= 0: return dst
    r, g, b, da = dst
    oa = a + da * (1 - a)
    if oa <= 0: return (0, 0, 0, 0)
    mix = lambda s, d: (s * a + d * da * (1 - a)) / oa
    return (mix(src[0], r), mix(src[1], g), mix(src[2], b), oa)

def lerp(c0, c1, t):
    return tuple(a + (b - a) * t for a, b in zip(c0, c1))

def winhello(n):
    """A blue tile with a white application window on it"""
    img = []
    u = n / 256
    for y in range(n):
        for x in range(n):
            px, py = x + 0.5, y + 0.5
            c = (0, 0, 0, 0)
            m = n * 0.06
            c = over(c, lerp((0x4C, 0x9A, 0xFF), (0x1E, 0x55, 0xD0), py / n),
                     cov(rrect(px, py, m, m, n - m, n - m, n * 0.2)))
            wx0, wy0, wx1, wy1 = n * 0.2, n * 0.27, n * 0.8, n * 0.76
            c = over(c, (0xFF, 0xFF, 0xFF), cov(rrect(px, py, wx0, wy0, wx1, wy1, max(1.0, 14 * u))))
            # title bar: the window's top, cut straight at its bottom edge
            c = over(c, (0x16, 0x2C, 0x5C), cov(rrect(px, py, wx0, wy0, wx1, wy1, max(1.0, 14 * u))) *
                     cov(py - (wy0 + n * 0.12)))
            if n >= 24:                        # caption dots and a text line
                for i in range(3):
                    d = math.hypot(px - (wx1 - n * (0.06 + 0.07 * i)), py - (wy0 + n * 0.06)) - n * 0.022
                    c = over(c, (0xFF, 0xFF, 0xFF), cov(d))
                c = over(c, (0x2E, 0xA0, 0x6A), cov(rrect(px, py, n * 0.28, n * 0.48, n * 0.44, n * 0.66, 4 * u)))
                for i in range(2):
                    yy = n * (0.5 + 0.1 * i)
                    c = over(c, (0xB8, 0xC4, 0xD8), cov(rrect(px, py, n * 0.5, yy, n * 0.72, yy + n * 0.05, 3 * u)))
            img.append(c)
    return img

def nova(n):
    """A glowing star: the NovaOS sample picture"""
    img = []
    for y in range(n):
        for x in range(n):
            px, py = x + 0.5 - n / 2, y + 0.5 - n / 2
            r = math.hypot(px, py) / (n / 2)
            c = (0, 0, 0, 0)
            t = min(1.0, r / 0.92)
            glow = lerp((0xFF, 0xE0, 0x6A), (0xE0, 0x3A, 0x8C), t)
            c = over(c, glow, cov(math.hypot(px, py) - n * 0.46))
            # four-point star: |x|^p + |y|^p style astroid
            ax, ay = abs(px) / (n * 0.40), abs(py) / (n * 0.40)
            s = (ax ** 0.5 + ay ** 0.5)
            d = (s - 1) * n * 0.2
            c = over(c, (0xFF, 0xFF, 0xF4), cov(d))
            img.append(c)
    return img

def png(n, img):
    raw = bytearray()
    for y in range(n):
        raw.append(0)
        for x in range(n):
            r, g, b, a = img[y * n + x]
            raw += bytes((round(r), round(g), round(b), round(a * 255)))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) +
            ch(b'IDAT', zlib.compress(bytes(raw), 9)) + ch(b'IEND', b''))

def dib(n, img):
    hdr = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, 0, 0, 0, 0, 0)
    xor, mask = bytearray(), bytearray()
    stride = (n + 31) // 32 * 4
    for y in reversed(range(n)):               # bottom-up
        row = bytearray(stride)
        for x in range(n):
            r, g, b, a = img[y * n + x]
            xor += bytes((round(b), round(g), round(r), round(a * 255)))
            if round(a * 255) == 0:
                row[x >> 3] |= 0x80 >> (x & 7)
        mask += row
    return hdr + xor + mask

def ico(draw):
    images = [(n, png(n, draw(n)) if n == 256 else dib(n, draw(n))) for n in SIZES]
    out = struct.pack('<HHH', 0, 1, len(images))
    off = 6 + 16 * len(images)
    body = b''
    for n, data in images:
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
        body += data
    return out + body

def sunset(w, h):
    """A sample picture (PNG): sky and three rolling hills"""
    img = []
    waves = [(0.62, 0.05, 1.3, 0.0, (0xC8, 0x3A, 0x5A)), (0.72, 0.04, 2.1, 1.0, (0xE8, 0x6A, 0x3A)),
             (0.84, 0.035, 1.7, 2.2, (0xF4, 0xB0, 0x3C))]
    for y in range(h):
        for x in range(w):
            t = y / h
            c = lerp((0x2A, 0x10, 0x4A), (0xF0, 0x80, 0x70), min(1.0, t / 0.7)) + (1.0,)
            sd = math.hypot(x - w * 0.7, y - h * 0.46) - h * 0.1
            c = over(c, (0xFF, 0xE8, 0xA0), cov(sd))
            for base, amp, freq, ph, col in waves:
                edge = h * (base + amp * math.sin(freq * 2 * math.pi * x / w + ph))
                c = over(c, col, cov(edge - y))
            img.append(c)
    return img

def png_rect(w, h, img):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            r, g, b, _ = img[y * w + x]
            raw += bytes((round(r), round(g), round(b)))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            ch(b'IDAT', zlib.compress(bytes(raw), 9)) + ch(b'IEND', b''))

PICTURES = {
    'userland/samples/Sunset.png': (sunset, 640, 400),
}

ICONS = {
    'userland/programs/winhello.ico': winhello,
    'userland/samples/Nova.ico': nova,
}

if __name__ == '__main__':
    for rel, draw in ICONS.items():
        path = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(ico(draw))
        print(f'{rel}: {os.path.getsize(path)} bytes')
    for rel, (draw, w, h) in PICTURES.items():
        path = os.path.join(ROOT, rel)
        open(path, 'wb').write(png_rect(w, h, draw(w, h)))
        print(f'{rel}: {os.path.getsize(path)} bytes')
