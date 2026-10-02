#!/usr/bin/env python3
"""Write the animated cursor anitest.exe tests with (userland/programs/anitest.ani)

    tools/mkani.py [OUT]

A busy spinner: 8 frames of 32 x 32, 32-bit .cur images with the hot spot
in the middle.  Frame k lights ring segment k and has a centre dot of
COLORS[k].  The "seq " chunk plays the frames backwards after the first
(0, 7, 6, ... 1) and "rate" holds step 0 for 10 jiffies, the rest for 5.
An INFO list (title, artist) is there to be skipped.  anitest.c checks
these numbers: keep the two in step.
"""
import math, os, struct, sys

W = H = 32
HOT = (16, 16)
COLORS = [0xE81123, 0xFF8C00, 0xFFB900, 0x10893E, 0x00B7C3, 0x0078D7, 0x8764B8, 0xE3008C]
SEQ = [0, 7, 6, 5, 4, 3, 2, 1]
RATE = [10, 5, 5, 5, 5, 5, 5, 5]


def frame_pixels(k):
    """ARGB rows, top first, anti-aliased by 4x4 supersampling"""
    px = []
    for y in range(H):
        for x in range(W):
            acc = [0.0, 0.0, 0.0, 0.0]          # premultiplied r, g, b, a
            for sy in range(4):
                for sx in range(4):
                    fx, fy = x + (sx + .5) / 4 - 16, y + (sy + .5) / 4 - 16
                    r = math.hypot(fx, fy)
                    col = None
                    if r <= 4.5:
                        col = COLORS[k]
                    elif 9 <= r <= 14:
                        seg = int(((math.degrees(math.atan2(fx, -fy)) + 360 + 22.5) % 360) // 45)
                        col = 0x0078D7 if seg == k else 0xC8C8C8
                        if r < 10 or r > 13:
                            col = 0x202020           # outline
                    if col is not None:
                        acc[0] += (col >> 16) & 255; acc[1] += (col >> 8) & 255; acc[2] += col & 255; acc[3] += 1
            a = acc[3] / 16
            if a:
                r, g, b = (int(acc[i] / acc[3] + .5) for i in range(3))
                px.append(int(a * 255 + .5) << 24 | r << 16 | g << 8 | b)
            else:
                px.append(0)
    return px


def cur_file(k):
    px = frame_pixels(k)
    bih = struct.pack('<IiiHHIIiiII', 40, W, H * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b''.join(struct.pack('<I', px[y * W + x]) for y in reversed(range(H)) for x in range(W))
    andm = bytes(((W + 31) // 32) * 4 * H)       # alpha says it all
    img = bih + xor + andm
    head = struct.pack('<HHH', 0, 2, 1)
    entry = struct.pack('<BBBBHHII', W, H, 0, 0, HOT[0], HOT[1], len(img), 6 + 16)
    return head + entry + img


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'userland', 'programs', 'anitest.ani')
    info = chunk(b'LIST', b'INFO' + chunk(b'INAM', b'NovaOS spinner\0') + chunk(b'IART', b'NovaOS\0'))
    anih = chunk(b'anih', struct.pack('<9I', 36, len(COLORS), len(SEQ), 0, 0, 0, 0, 6, 1 | 2))
    rate = chunk(b'rate', struct.pack(f'<{len(RATE)}I', *RATE))
    seq = chunk(b'seq ', struct.pack(f'<{len(SEQ)}I', *SEQ))
    fram = chunk(b'LIST', b'fram' + b''.join(chunk(b'icon', cur_file(k)) for k in range(len(COLORS))))
    body = b'ACON' + info + anih + rate + seq + fram
    with open(out, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


if __name__ == '__main__':
    main()
