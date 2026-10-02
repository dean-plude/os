- **Text**: `novatext.dll`, the text core built once and shared, carries
  HarfBuzz (shaping) and FreeType (fonts).  Uniscribe (`usp10`) itemizes
  text by script and direction and shapes it with HarfBuzz, and GDI's
  `ExtTextOut` sends complex scripts through it, as Windows' LPK does, so
  Arabic, Hebrew and the Indic scripts join, reorder and run right to left.
  Arabic and Devanagari draw with Noto Sans; GDI falls back to them by
  script.
