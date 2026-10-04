## DirectWrite: IDWriteFactory2 and IDWriteFactory3

Steam's browser (Chromium, CEF 126) started its first page and then the
browser process crashed: Chromium asks its DirectWrite factory for
`IDWriteFactory2` and `IDWriteFactory3`, the interfaces Windows 8.1 and
Windows 10 added, uses the answer without checking it, and NovaOS's
factory only knew the first two versions.  Chromium's font lookup table
then called `GetSystemFontCollection` through a null `IDWriteFactory3`,
and Steam started the browser again, over and over.  NovaOS's DirectWrite
now has Windows 10's font model; it is NovaOS's own code (Wine's
DirectWrite is LGPL), written from the interfaces in the Windows SDK
headers.

- **`IDWriteFactory2`**: the system font fallback (`GetSystemFontFallback`)
  and fallback builders, `TranslateColorGlyphRun`, rendering params with a
  grid-fit mode, and glyph run analysis with an anti-aliasing mode
  (grayscale coverage comes in the 1x1 texture, the way Skia reads it).
- **`IDWriteFactory3`**: font face references (from a font file or a
  path), the system font set, font set builders, collections made from
  font sets, `GetSystemFontCollection` as an `IDWriteFontCollection1`,
  rendering mode 1 (`NATURAL_SYMMETRIC_DOWNSAMPLED`) and the font download
  queue, which is always empty because every font is a local file.
- **Font sets** (`fontset.c`) carry each font's properties (weight/stretch/
  style and typographic family and face names, full, Win32 and PostScript
  names, weight, stretch, style), list their values and filter on them,
  which is how Chromium matches `local()` fonts by unique name.  Matching
  a family in the system set knows the default names ("Segoe UI",
  "Consolas"...) that Inter and DejaVu Sans Mono stand for.
- **Font fallback**: `MapCharacters` keeps the base font for the
  characters it has and picks a system font that has the rest (Noto Sans
  Arabic, Devanagari...), with spaces and marks staying with their
  neighbours; text no font has comes back with no font.  Built fallbacks
  try their own mappings (ranges, families, an optional base family)
  first, then the system's if it was added.
- **Fonts and faces**: collections are `IDWriteFontCollection1`, families
  `IDWriteFontFamily1`, lists `IDWriteFontList1`, fonts `IDWriteFont3` and
  faces `IDWriteFontFace3` (family and face names, informational strings,
  face references, locality).  Color fonts (`COLR` version 0 with `CPAL`)
  report their palettes, and `TranslateColorGlyphRun` splits a run into
  its color layers; fonts without them answer `DWRITE_E_NOCOLOR`.
- **Test**: the new `tools/dw3test`, in the graphics suite, goes through
  all of this the way Chromium and Skia do (60 checks, 64- and 32-bit).
- **Steam**: the browser no longer crashes and is no longer restarted.
  Under emulation it starts its GPU, network and storage processes,
  creates Steam's first browser and launches its page process, which is
  as far as the corpus test's time reaches
  ([compatibility.md](../compatibility.md#steam)).
