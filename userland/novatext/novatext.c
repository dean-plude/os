/*
 * novatext.dll — NovaOS's text core, built once and shared: HarfBuzz
 * (third_party/harfbuzz) shapes text and FreeType (third_party/freetype)
 * loads and rasterizes fonts.  The DLL exports both libraries' C APIs
 * (hb_*, FT_*) for Uniscribe (usp10.dll), DirectWrite and Direct2D.
 */
int _fltused = 0x9875;          /* floating point in use (the compiler references it) */
