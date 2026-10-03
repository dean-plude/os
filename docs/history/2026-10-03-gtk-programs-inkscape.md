## GTK programs: Inkscape

Phase 20.4 starts with Inkscape 0.91, unmodified: the GTK 2 build in
conda-forge's win-64 channel (a MinGW build carrying its own GTK 2,
cairo, pango and about 70 DLLs).  Inkscape 1.x is a GTK 3 program whose
downloads (inkscape.org, MSYS2's mirrors, GitLab's artifacts) are not
reachable from CI, so it is not tested yet.  Inkscape now starts with a
new document and shows its menus, tool bars, toolbox, rulers, canvas,
palette and status bar.  What was missing:

- **More DLLs per process.**  The loader stopped at 64 modules ("too many
  DLLs"); a GTK program brings about 70.  The limit is 128 now, and the
  loader information page ntdll reads (`NOVA_LDR_INFO`) grew to six
  pages, moving the process parameters and stubs after it.  GTK loads its
  pixbuf loaders and theme engine by relative paths with forward slashes
  (`lib/gdk-pixbuf-2.0/2.10.0/loaders/...`), which the loader now takes as
  paths.
- **Regions with more than one rectangle.**  gdi32 kept every region as
  its bounding box.  GDK 2 draws client-side child windows and clips
  each toplevel paint to the window minus its children, so the bounding
  box let the toplevel's background erase the canvas.  Regions
  (`userland/gdi32/region.c`) now keep their rectangles: `CombineRgn`
  with all five modes, `ExtCreateRegion`, `GetRegionData`, `PtInRegion`,
  `RectInRegion`, `EqualRgn`, `OffsetRgn`, `FillRgn` and `PaintRgn`; a
  DC's clip keeps up to 128 rectangles (beyond that, their bounding box),
  and fills and blits draw piece by piece.
- **Text under a world transform.**  cairo's win32 backend creates its
  fonts 32 times too large and draws them through a `GM_ADVANCED` world
  transform of 1/32, asking `GetGlyphOutline` for metrics in device
  space.  `ExtTextOut` (positions, the clip rectangle and the `lpDx`
  advances) and `GetGlyphOutline` now apply a scale-and-translate
  transform, so the text is the right size and spacing instead of blank
  or spread out.
- **`StretchDIBits` with negative extents.**  cairo uploads image surfaces
  with both heights negative and a source y counted from the bottom of
  the image, top-down images included, as Windows does.  gdi32 took the
  source y from the top and lost a row of each band; it now follows
  Windows (an extent of -h from y covers y-h+1..y, and the image is
  mirrored only when the destination and source signs differ).
- **Smaller pieces**: `mscms.dll` (`GetColorDirectory` and an empty
  `EnumColorProfiles`), `Arc`/`Pie`/`Chord`, `CreateBitmapIndirect`,
  `MaskBlt`, `GetNearestPaletteIndex`, `ResetDC`, failing enhanced-metafile
  calls, `SetCriticalSectionSpinCount` (Inkscape's GLib crashed on its
  stub), `GetCPInfoExA`, `Module32First/Next`, `GetCurrentHwProfileA`,
  `AssocQueryKeyW`, `ExtractIconExA`, `SHAppBarMessage`, and msvcrt's
  `swscanf`/`vswscanf` exports, `_splitpath`, `_wsplitpath` and
  `__lconv_init`.
- **Tests**: Inkscape in the nightly corpus (`tests/appcorpus/880-inkscape.py`):
  its new-document window's screenshot must match
  `tests/reference/inkscape.png`.

Still open: GDK's monochrome cursors (`CreateDIBSection` takes only 24-
and 32-bit DIBs, so GTK falls back to the default pointer), the hicolor
icon theme warning, and no Wintab tablets.
