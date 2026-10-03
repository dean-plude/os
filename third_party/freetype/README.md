# FreeType

The font engine in `novatext.dll` (`userland/novatext`): loading,
hinting and rasterizing fonts, outlines and strokes for Uniscribe,
DirectWrite and Direct2D.

Version: **FreeType 2.13.3** (Ubuntu's `freetype_2.13.3+dfsg.orig.tar.xz`),
unmodified.  Kept: `include/` and the modules `base`, `autofit`,
`truetype`, `type1`, `cff`, `cid`, `psaux`, `psnames`, `pshinter`, `sfnt`,
`smooth`, `raster` and `winfonts`; `userland/novatext/nova_ftmodule.h`
lists them and `nova_ftoption.h` turns zlib off.

FreeType is dual-licensed; NovaOS uses it under the FreeType License
(`FTL.TXT`, BSD-style with a credit clause; see `LICENSE.TXT`).  Portions
of this software are copyright © 2024 The FreeType Project
(www.freetype.org).  All rights reserved.
