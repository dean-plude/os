#!/usr/bin/env python3
"""Draw d2dtest's scene with Skia (pip install skia-python) and save it as
tools/d2dtest/d2dref.png, the image d2dtest's Direct2D drawing must match.

    tools/d2dtest/reference.py [OUT.png]

Every shape here is drawn in tools/d2dtest/d2dtest.c the same way; change
both together and re-run this script.
"""
import math, os, sys
import skia

W, H = 400, 300


def color(r, g, b, a=1.0):
    return skia.Color4f(r, g, b, a)


def paint(c, stroke=None, cap=skia.Paint.kButt_Cap, join=skia.Paint.kMiter_Join):
    p = skia.Paint(AntiAlias=True)
    p.setColor4f(c)
    if stroke:
        p.setStyle(skia.Paint.kStroke_Style)
        p.setStrokeWidth(stroke)
        p.setStrokeCap(cap)
        p.setStrokeJoin(join)
        p.setStrokeMiter(10)
    return p


def star(cx, cy, ro, ri):
    path = skia.Path()
    for i in range(10):
        r = ro if i % 2 == 0 else ri
        a = -math.pi / 2 + i * math.pi / 5
        x, y = cx + r * math.cos(a), cy + r * math.sin(a)
        path.moveTo(x, y) if i == 0 else path.lineTo(x, y)
    path.close()
    return path


def rounded(l, t, r, b, rad):
    """clockwise from the top edge, corners as SVG arcs (as the D2D sink's AddArc)"""
    p = skia.Path()
    p.moveTo(l + rad, t)
    p.lineTo(r - rad, t)
    p.arcTo(rad, rad, 0, skia.Path.ArcSize.kSmall_ArcSize, skia.PathDirection.kCW, r, t + rad)
    p.lineTo(r, b - rad)
    p.arcTo(rad, rad, 0, skia.Path.ArcSize.kSmall_ArcSize, skia.PathDirection.kCW, r - rad, b)
    p.lineTo(l + rad, b)
    p.arcTo(rad, rad, 0, skia.Path.ArcSize.kSmall_ArcSize, skia.PathDirection.kCW, l, b - rad)
    p.lineTo(l, t + rad)
    p.arcTo(rad, rad, 0, skia.Path.ArcSize.kSmall_ArcSize, skia.PathDirection.kCW, l + rad, t)
    p.close()
    return p


def scene(c):
    c.clear(skia.ColorWHITE)
    # row 1: solid fill, stroked rectangle, ellipse
    c.drawRect(skia.Rect(20, 20, 120, 80), paint(color(0.9, 0.1, 0.1)))
    c.drawRect(skia.Rect(140, 20, 240, 80), paint(color(0.1, 0.2, 0.9), stroke=4))
    c.drawOval(skia.Rect(260, 20, 380, 80), paint(color(0.1, 0.7, 0.2)))
    # row 2: linear gradient, radial gradient, rotated translucent rectangle
    lin = paint(color(0, 0, 0))
    lin.setShader(skia.GradientShader.MakeLinear([(20, 0), (200, 0)], [0xFFFFDD00, 0xFF0033CC]))
    c.drawRect(skia.Rect(20, 100, 200, 160), lin)
    rad = paint(color(0, 0, 0))
    m = skia.Matrix()
    m.setScale(70, 40)
    m.postTranslate(300, 130)
    rad.setShader(skia.GradientShader.MakeRadial((0, 0), 1, [0xFFFFFFFF, 0xFF8020A0], None,
                                                 skia.TileMode.kClamp, 0, m))
    c.drawOval(skia.Rect(230, 90, 370, 170), rad)
    c.save()
    c.rotate(30, 110, 130)
    c.drawRect(skia.Rect(80, 120, 140, 140), paint(color(0, 0, 0, 0.5)))
    c.restore()
    # row 3: star filled and stroked with round joins, dashed rounded path, bitmap
    s = star(90, 235, 50, 20)
    c.drawPath(s, paint(color(1, 0.6, 0)))
    c.drawPath(s, paint(color(0, 0, 0), stroke=3, join=skia.Paint.kRound_Join))
    dash = paint(color(0.25, 0.25, 0.25), stroke=5)
    dash.setPathEffect(skia.DashPathEffect.Make([10, 10], 0))
    c.drawPath(rounded(200, 185, 300, 280, 15), dash)
    px = []
    for y in range(4):
        for x in range(4):
            px.append(0xFF2060E0 if (x + y) % 2 else 0xFFF0C020)
    import array
    data = array.array('I', px).tobytes()
    img = skia.Image.frombytes(data, (4, 4), skia.kBGRA_8888_ColorType, skia.kPremul_AlphaType)
    c.drawImageRect(img, skia.Rect(0, 0, 4, 4), skia.Rect(320, 190, 380, 250),
                    skia.SamplingOptions(skia.FilterMode.kNearest), None)
    # a round-capped line along the bottom
    c.drawLine(20, 292, 380, 292, paint(color(0, 0.5, 0.5), stroke=6, cap=skia.Paint.kRound_Cap))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'd2dref.png')
    surface = skia.Surface(W, H)
    with surface as c:
        scene(c)
    surface.makeImageSnapshot().save(out, skia.kPNG)
    print('wrote', out)


if __name__ == '__main__':
    main()
