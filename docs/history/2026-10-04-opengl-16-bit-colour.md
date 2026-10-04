## OpenGL in 16-bit colour: Chocolate Doom in the right colours

Chocolate Doom (SDL2) drew Freedoom in red, pink and yellow noise on Mesa
3D's OpenGL.  SDL2 asks OpenGL for at least 3:3:2 bits of colour, and
Mesa's `wglChoosePixelFormatARB` answers with its closest window format,
4:4:4:4 (16 bits).  Mesa presents such a frame to the window as a 16-bit
DIB whose `BITMAPV5HEADER` carries `BI_BITFIELDS` masks (`0x0F00`, `0x00F0`,
`0x000F`), and gdi32's `StretchDIBits` read every 16-bit DIB as either
5:6:5 or 5:5:5, so each 4-bit channel landed in the wrong bits.

- gdi32 reads 16- and 32-bit DIBs by their own masks (`BI_BITFIELDS` and
  `BI_ALPHABITFIELDS`, any header version), as Windows does: 4:4:4:4,
  5:6:5, 5:5:5, 10:10:10:2 and red-first 8:8:8 frames all show in their
  colours.  The fast path for presenting frames only copies 32-bit DIBs
  whose masks are the usual 0x00RRGGBB.
- Test: the graphics suite's `gltest colors` draws four coloured bars in
  every colour depth Mesa offers a window and reads them back
  (`tests/selftest/graphics/105-gltest-colors.py`), 32- and 64-bit.

Chocolate Doom now shows Freedoom in its own colours, in the 4 bits per
channel SDL2 asked for (as on Windows with Mesa).
