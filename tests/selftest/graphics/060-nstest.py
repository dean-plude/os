# nstest: NetSurf (Phase 19.8) shows an SVG image, an svg element written
# inline in the HTML and a list a script builds, and redraws a page a script
# changes after layout.  nstest writes the page and starts NetSurf; this
# test looks at the screen: the SVG's red circle, blue rectangle and green
# outline, the inline SVG's magenta square and cyan circle (at the size its
# width, height and viewBox give), the three list items (orange, purple,
# teal) and the page's yellow box, then clicks the box (its script
# turns it green and bigger) and waits for the green box with the yellow
# one gone, then closes NetSurf with Alt+F4 (nstest passes when NetSurf
# exits normally).  A screenshot of each state is kept in --out.
import os, re, struct, sys, time, zlib

SEEN = {'why': 'nstest never started NetSurf'}     # why the screen check failed, for check()
LOGICAL_W = 1280            # the desktop's logical width (novarun clicks in it); the boot log's
                            # "[GDI] WxH device, scale Nx -> LWxLH logical" line overrides it


def png_rgb(path):
    """(width, height, rows of RGB bytes) of QEMU's screendump PNG"""
    data = open(path, 'rb').read()
    pos, idat, w, h, ctype = 8, b'', 0, 0, 2
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, _, ctype = struct.unpack('>IIBB', body[:10])
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, rows, prev, stride = zlib.decompress(idat), [], bytearray(w * bpp), w * bpp
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        rows.append(bytes(line) if bpp == 3 else bytes(v for i, v in enumerate(line) if i % 4 != 3))
    return w, h, rows


COLOURS = {                 # name: test on (r, g, b)
    'red': lambda r, g, b: r > 230 and g < 25 and b < 25,
    'blue': lambda r, g, b: b > 230 and r < 25 and g < 25,
    'outline': lambda r, g, b: g > 140 and g < 180 and r < 25 and b < 25,
    'yellow': lambda r, g, b: r > 235 and g > 235 and b < 20,
    'green': lambda r, g, b: g > 235 and r < 20 and b < 20,
    'magenta': lambda r, g, b: r > 235 and b > 235 and g < 20,
    'cyan': lambda r, g, b: g > 235 and b > 235 and r < 20,
    'orange': lambda r, g, b: r > 235 and 110 < g < 145 and b < 20,
    'purple': lambda r, g, b: 110 < r < 145 and g < 20 and b > 235,
    'teal': lambda r, g, b: r < 20 and 110 < g < 145 and 110 < b < 145,
}
INLINE = ('magenta', 'cyan')
LIST = ('orange', 'purple', 'teal')


def census(path):
    """{colour: (pixel count, centre x, centre y)} on the screenshot (every
    second pixel), the count in logical pixels and the centre in logical
    desktop coordinates"""
    w, h, rows = png_rgb(path)
    found = {k: [0, 0, 0] for k in COLOURS}
    for y in range(0, h, 2):
        row = rows[y]
        for x in range(0, w, 2):
            r, g, b = row[3 * x], row[3 * x + 1], row[3 * x + 2]
            for k, test in COLOURS.items():
                if test(r, g, b):
                    s = found[k]
                    s[0] += 1
                    s[1] += x
                    s[2] += y
    scale = LOGICAL_W / w
    per = (w / LOGICAL_W / 2) ** 2      # samples per logical pixel (1 at a 2x scale)
    return {k: (int(n / per), int(sx / n * scale) if n else 0, int(sy / n * scale) if n else 0)
            for k, (n, sx, sy) in found.items()}


def wait_for(nova, path, ok, timeout):
    """screenshots until @ok(census) holds; the last census"""
    end = time.time() + timeout
    while True:
        nova.shot(path)
        c = census(path)
        if ok(c) or time.time() > end:
            return c
        time.sleep(2)


def look_and_click(nova):
    global LOGICAL_W
    SEEN['why'] = None
    m = re.search(r'\[GDI\] \d+x\d+ device, scale \S+ -> (\d+)x\d+ logical', nova.boot_log)
    if m:
        LOGICAL_W = int(m.group(1))
    out = os.path.join(getattr(sys.modules['__main__'], 'OUT', '.'), 'nstest')   # selftest.py's --out
    before = wait_for(nova, out + '-before.png',
                      lambda c: all(c[k][0] > 200 for k in ('red', 'blue', 'yellow') + INLINE + LIST)
                      and c['outline'][0] > 50, 240)
    miss = [k for k in ('red', 'blue', 'outline', 'yellow') if before[k][0] < (50 if k == 'outline' else 200)]
    # (one census sample is one logical pixel: the inline SVG's square is
    # 20 viewBox units = 80 pixels a side, its circle 64 pixels across; a
    # list item is 100 x 20 pixels)
    sq, circ = before['magenta'][0], before['cyan'][0]
    if miss:
        SEEN['why'] = 'no ' + ', '.join(miss) + ' on the screen (the SVG image or the page did not show)'
    elif not sq or not circ:
        SEEN['why'] = 'no inline SVG on the screen (magenta %d, cyan %d pixels)' % (sq, circ)
    elif not (5000 < sq < 8000 and 2600 < circ < 4000):
        SEEN['why'] = 'the inline SVG has the wrong size (magenta %d of 6400, cyan %d of 3217 pixels)' % (sq, circ)
    elif [k for k in LIST if not 1500 < before[k][0] < 2500]:
        SEEN['why'] = 'the script-built list is wrong (%s of 2000 pixels each)' % \
            ', '.join('%s %d' % (k, before[k][0]) for k in LIST)
    elif before['green'][0] > 50:
        SEEN['why'] = 'the box was green before the click'
    else:
        n, x, y = before['yellow']
        nova.click(x, y)
        after = wait_for(nova, out + '-after.png',
                         lambda c: c['green'][0] > before['yellow'][0] and c['yellow'][0] < 20, 120)
        if after['green'][0] <= before['yellow'][0] or after['yellow'][0] >= 20:
            SEEN['why'] = 'the page did not redraw the box the script changed (green %d, yellow %d pixels)' % \
                (after['green'][0], after['yellow'][0])
        elif after['red'][0] < 200 or after['blue'][0] < 200:
            SEEN['why'] = 'the SVG image went missing when the page was laid out again'
        elif [k for k in INLINE + LIST if after[k][0] < 1000]:
            SEEN['why'] = 'the inline SVG or the list went missing when the page was laid out again'
    time.sleep(1)
    nova.qmp.key('alt', 'f4')


def screen_check(nova):
    return SEEN.get('why')


TESTS = [
    Test('nstest', 'nstest', [r'nstest: \d+ passed, 0 failed'], timeout=600,
         acts=[(r'NetSurf shows the test page', look_and_click)], check=screen_check),
]
