# HarfBuzz

The text-shaping engine in `novatext.dll` (`userland/novatext`), used by
Uniscribe (`usp10.dll`) and shared with DirectWrite and Direct2D.

Version: **HarfBuzz 11.2.1**, from
<https://github.com/harfbuzz/harfbuzz/releases/tag/11.2.1>
(`harfbuzz-11.2.1.tar.xz`).  Only the files `src/harfbuzz.cc` includes are
kept (no subsetter, Cairo, GLib, ICU or Wasm parts), unmodified.
Licensed under the "Old MIT" licence (see `COPYING`).

`tools/build_userland.py` compiles `src/harfbuzz.cc` as C++17 with clang
for the Windows targets, against libc++'s headers, with `HB_NO_MT`,
`HB_NO_MMAP` and `HAVE_FREETYPE`.
