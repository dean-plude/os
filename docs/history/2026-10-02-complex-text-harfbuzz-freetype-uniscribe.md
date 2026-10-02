## Complex text: HarfBuzz, FreeType and Uniscribe

Phase 19.1.  Arabic, Hebrew and the Indic scripts are shaped: letters join
into their contextual forms, ligate, reorder and run right to left.

- **`novatext.dll`** (`userland/novatext`) is the text core, built once and
  shared by Uniscribe, DirectWrite and Direct2D: **HarfBuzz 11.2.1**
  (`third_party/harfbuzz`, MIT) and **FreeType 2.13.3**
  (`third_party/freetype`, the FreeType License; TrueType, OpenType/CFF,
  Type 1, CID and `.fon` drivers, the smooth and mono rasterizers, the
  auto-hinter and the stroker).  It exports both libraries' C APIs (`hb_*`,
  `FT_*`).  No MIT Uniscribe or HarfBuzz-free shaper fitted, so this reuses
  the standard pair.  HarfBuzz is C++: `tools/build_userland.py` compiles it
  with clang against libc++'s headers (`libc++-dev`; nothing of libc++ is
  linked, and HarfBuzz needs no C++ runtime).  For that, the userland's
  `math.h` and `stdlib.h` gained the C++ overloads the Windows SDK's have,
  `locale.h` Windows' full `lconv`, and `include/cxx/functional` a slim
  `<functional>`.
- **`usp10.dll`** (`userland/usp10/usp10.c`) is rewritten on it.
  `ScriptItemize` splits text by script (HarfBuzz's Unicode data) and by
  bidirectional level (a compact UAX #9: strong letters, European and
  Arabic numbers, neutrals between them).  `ScriptShape` reads the DC's
  font through `GetFontData`, shapes the run with HarfBuzz at gdi32's
  pixel size and returns glyphs in visual order, logical clusters and
  visual attributes; it reports `USP_E_SCRIPT_NOT_IN_FONT` when the font
  lacks a complex script.  `ScriptPlace` hands out HarfBuzz's advances and
  mark offsets, `ScriptTextOut` draws marks at their offsets, and
  `ScriptCPtoX`, `ScriptXtoCP` and `ScriptGetLogicalWidths` handle
  right-to-left clusters.  New: `ScriptShapeOpenType`,
  `ScriptPlaceOpenType` (OpenType features), `ScriptItemizeOpenType`'s
  real script tags, `ScriptGetFontScriptTags`, `ScriptStringGetOrder`.
  The `ScriptString*` layer shapes each run with a fallback font when the
  DC's lacks its script (`SSA_FALLBACK`), lays the runs out in visual
  order and draws them all on the DC font's baseline.
- **GDI**: `ExtTextOut`, `TextOut` and `GetTextExtentPoint32` send text
  with complex-script characters through Uniscribe, as Windows' LPK does
  (`ETO_GLYPH_INDEX` and `ETO_IGNORELANGUAGE` skip it; `ETO_RTLREADING`
  makes the line right to left).  GDI gained the faces Noto Sans Arabic
  and Noto Sans Devanagari (`C:\Windows\Fonts`), also under Windows'
  names for those scripts (Traditional Arabic, Mangal, Nirmala UI...).
- **`usptest`** (in the CI core suite, 64- and 32-bit) checks itemizing,
  the Arabic contextual forms and lam-alef ligature, the Devanagari kssa
  conjunct with its reordered i sign and a half form, against the glyphs
  HarfBuzz gives on the build host, and that `ExtTextOut` draws mixed
  Latin, Arabic and Devanagari lines pixel for pixel as `ScriptStringOut`
  does.
