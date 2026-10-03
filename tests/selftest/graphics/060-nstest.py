# nstest: NetSurf (Phase 19.8) shows an SVG image, an svg element written
# inline in the HTML, a list a script builds, an SVG without a size (at the
# default 300 x 150) and an iframe holding a frameset page, and redraws a
# page a script changes after layout.  nstest writes the page and starts NetSurf; this
# test looks at the screen: the SVG's red circle, blue rectangle and green
# outline, the inline SVG's magenta square and cyan circle (at the size its
# width, height and viewBox give), the three list items (orange, purple,
# teal), the default-size SVG's olive rectangle, the iframe's two frames
# (brown and pink) and the page's yellow box, then clicks the box (its
# script turns it green and bigger) and waits for the green box with the
# yellow one gone and everything else still there, then closes NetSurf with Alt+F4 (nstest passes when NetSurf
# exits normally).  A screenshot of each state is kept in --out.  Then
# nstest opens four pages a script changes twelve times (a long page, one
# with iframes, one with positioned boxes, one with floats), each laid out
# from the changed box and then again with NetSurf checking every third
# such layout against a full one; the test takes a screenshot of each run
# (nstest-PAGE-incremental.png, nstest-PAGE-check.png) and requires the
# two of a page to be identical above NetSurf's status bar.
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
    'olive': lambda r, g, b: 110 < r < 145 and 110 < g < 145 and b < 20,
    'brown': lambda r, g, b: 110 < r < 145 and 50 < g < 80 and b < 20,
    'pink': lambda r, g, b: r > 235 and g < 20 and 110 < b < 145,
}
INLINE = ('magenta', 'cyan')
LIST = ('orange', 'purple', 'teal')
FRAMES = ('brown', 'pink')


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
                      lambda c: all(c[k][0] > 200 for k in ('red', 'blue', 'yellow', 'olive') + INLINE + LIST + FRAMES)
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
    elif not 38000 < before['olive'][0] < 52000:
        SEEN['why'] = 'the SVG without a size is not at the default 300 x 150 (olive %d of 45000 pixels)' % \
            before['olive'][0]
    elif [k for k in FRAMES if before[k][0] < 5000]:
        SEEN['why'] = 'the frames in the iframe did not show (%s pixels)' % \
            ', '.join('%s %d' % (k, before[k][0]) for k in FRAMES)
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
        elif after['olive'][0] < 38000 or [k for k in FRAMES if after[k][0] < 5000]:
            SEEN['why'] = 'the default-size SVG or the frames went missing when the page was laid out again'
    park_pointer(nova)
    time.sleep(1)
    nova.qmp.key('alt', 'f4')


def park_pointer(nova):
    """the pointer to the screen's bottom-right corner, out of the page
    screenshots compared later (NovaOS hides and shows it as it redraws)"""
    for _ in range(40):
        nova.hmp('mouse_move 100 100')
        time.sleep(0.01)


def screen_check(nova):
    return SEEN.get('why')


PAGES = ('long', 'iframe', 'positioned', 'floats')
SHOTS = {}                  # (page, mode): screenshot path


def png_rows(path):
    """(width, height, the decompressed scanlines with their filter bytes)"""
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
    stride = w * (4 if ctype == 6 else 3) + 1
    raw = zlib.decompress(idat)
    return w, h, [raw[y * stride:(y + 1) * stride] for y in range(h)]


def same_page(a, b):
    """None when two screenshots show the same page, else how they differ.
    The rows from the top of the screen down to NetSurf's status bar are
    compared (the status bar shows the load time, the taskbar a clock);
    the same encoder filters the same rows the same way, so equal
    scanlines mean equal pixels"""
    wa, ha, ra = png_rows(a)
    wb, hb, rb = png_rows(b)
    if (wa, ha) != (wb, hb):
        return 'the screens differ in size'
    bottom = ha * 1335 // 1600              # the page area ends above the status bar
    bad = [y for y in range(bottom) if ra[y] != rb[y]]
    if bad:
        return '%d screen rows differ (the first at y %d of %d)' % (len(bad), bad[0], ha)
    return None


def page_shot(page, mode):
    def act(nova):
        out = os.path.join(getattr(sys.modules['__main__'], 'OUT', '.'), 'nstest-%s-%s.png' % (page, mode))
        nova.shot(out)
        SHOTS[(page, mode)] = out
    return act


def all_checks(nova):
    why = screen_check(nova)
    if why:
        return why
    for page in PAGES:
        a, b = SHOTS.get((page, 'incremental')), SHOTS.get((page, 'check'))
        if not a or not b:
            return 'no screenshot of the %s page' % page
        diff = same_page(a, b)
        if diff:
            return 'the %s page laid out from the changed box looks different from the whole-page layout: %s' % \
                (page, diff)
    return None


TESTS = [
    Test('nstest', 'nstest', [r'nstest: \d+ passed, 0 failed'], timeout=1500,
         acts=[(r'NetSurf shows the test page', look_and_click)] +
         [(r'nstest: %s %s shown' % (p, m), page_shot(p, m)) for p in PAGES for m in ('incremental', 'check')],
         check=all_checks),
]
