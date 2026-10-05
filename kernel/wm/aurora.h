/* Aurora's procedural sunrise: no bitmap allocation or disk access during
 * painting. Coordinates are relative to r so previews and other monitors
 * use exactly the same scene as the primary display. */
#pragma once

static void aurora_wallpaper(GdiRect r)
{
    int horizon = r.y + r.h * 68 / 100;
    GdiGradientV(RECT(r.x, r.y, r.w, horizon - r.y),
                 GDI_C(29, 48, 79), GDI_C(242, 180, 140));
    GdiGradientV(RECT(r.x, horizon, r.w, r.y + r.h - horizon),
                 GDI_C(110, 127, 159), GDI_C(13, 30, 49));
    if (r.w < 8 || r.h < 8) return;
    /* Planet, sunrise and its reflection in the water. */
    int sunx = r.x + r.w * 71 / 100;
    int suny = r.y + r.h * 53 / 100;
    int radius = r.h / 35;
    if (radius < 1) radius = 1;
    int planet = r.h / 4 < r.w / 4 ? r.h / 4 : r.w / 4;
    GdiFillCircle(r.x + r.w * 55 / 100, r.y + planet,
                  planet, GDI_C(146, 157, 185));
    GdiFillCircle(r.x + r.w * 55 / 100 - planet / 10,
                  r.y + planet, planet * 9 / 10,
                  GDI_C(75, 96, 133));
    GdiFillCircle(sunx, suny, radius, GDI_C(255, 239, 186));
    for (int i = 0; i < 20; i++) {
        int width = radius + i * r.w / 240;
        int ry = horizon + i * (r.h / 90 + 1);
        int rh = 1 + r.h / 350;
        if (ry + rh > r.y + r.h) break;
        GdiAlphaFill(RECT(sunx - width, ry, width * 2, rh),
                     GDI_C(255, 206, 155), 125 - i * 5);
    }
    /* A fixed seed keeps the skyline and stars stable across redraws. */
    UINT32 seed = 2030;
    for (int i = 0; i < 80; i++) {
        seed = seed * 1664525u + 1013904223u;
        int x = r.x + (int)(seed % (UINT32)r.w);
        seed = seed * 1664525u + 1013904223u;
        int y = r.y + (int)(seed % (UINT32)(r.h / 3 + 1));
        GdiAlphaFill(RECT(x, y, 1, 1), GDI_WHITE, 100);
    }
    for (int layer = 0; layer < 3; layer++) {
        int base = horizon + layer * r.h / 13;
        int step = r.w / 40 + 1;
        GdiColor city = layer == 0 ? GDI_C(99, 111, 145) :
                        layer == 1 ? GDI_C(46, 66, 96) : GDI_C(19, 37, 59);
        for (int x = r.x; x < r.x + r.w; x += step) {
            seed = seed * 1664525u + 1013904223u;
            int height = r.h / 16 + (int)(seed % (UINT32)(r.h / 4 + 1));
            int width = step * 3 / 4;
            if (width < 1) width = 1;
            if (x + width > r.x + r.w) width = r.x + r.w - x;
            GdiFillRect(RECT(x, base - height, width, height), city);
            if ((seed & 3) == 0)
                GdiFillRect(RECT(x + width / 2, base - height - r.h / 25,
                                 1, r.h / 25), city);
            if (layer > 0 && width > 5) {
                for (int y = base - height + 5; y < base - 3; y += 9)
                    GdiAlphaFill(RECT(x + 3, y, width - 5, 1),
                                 GDI_C(255, 208, 148), 110);
            }
        }
    }
    /* Foreground riverbank; the city remains visible above the dock. */
    GdiGradientV(RECT(r.x, r.y + r.h * 92 / 100, r.w,
                      r.h - r.h * 92 / 100),
                 GDI_C(21, 40, 58), GDI_C(8, 19, 31));
}
