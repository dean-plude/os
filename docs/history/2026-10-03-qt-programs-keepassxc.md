## Qt programs: KeePassXC

Phase 20.3 starts with KeePassXC 2.7.12 (the portable Qt 5 zip from its
GitHub releases), unmodified.  With `msvcp140.dll` in place it already
opened its window, but every widget's text was blank, and opening a
database ended the program.  It now opens and unlocks a password
database and shows its groups, entries and the selected entry's details.
What was missing:

- **`GetGlyphOutline`** was a stub returning `GDI_ERROR`.  Qt's GDI font
  engine measures each glyph with it (`GGO_METRICS | GGO_GLYPH_INDEX`) and
  draws text from the coverage bitmaps it returns, so without it Qt drew
  nothing.  It now gives the metrics of gdi32's cached glyphs, their
  coverage as `GGO_BITMAP` and `GGO_GRAY2/4/8_BITMAP` (rows padded to
  four bytes), and their outlines as `GGO_NATIVE` (one `TTPOLYGONHEADER`
  per contour, quadratic `TT_PRIM_QSPLINE` segments in 16.16 pixels,
  straight from stb_truetype's shapes), which `wglUseFontOutlines` can use
  too.  The transform argument is taken as the identity.
- **`SetSecurityInfo` on `GetCurrentProcess()`**: KeePassXC replaces its
  own process's DACL so other programs cannot read its memory, and printed
  "Unable to disable core dumps" because `NtSetSecurityObject` did not
  know the pseudo-handles.  The current process and thread pseudo-handles
  are valid there now (their descriptors are not kept yet, as for other
  process handles).
- **HSTRINGs in Windows' layout.**  C++/WinRT does not call
  `WindowsCreateString`: it builds HSTRINGs itself, in the layout Windows
  uses internally (flags, length, two padding words, the character
  pointer; heap strings carry their reference count after that, from the
  process heap), and passes them to combase.  NovaOS's HSTRING kept the
  pointer at offset 8, so `RoGetActivationFactory` read a null class name
  and crashed.  `userland/ole32/winrt.c` now uses that same layout.
- **`Windows.Security.Credentials.KeyCredentialManager`** (Windows Hello)
  activates (`userland/ole32/winrt_classes.c`, the first runtime class
  NovaOS answers for).  KeePassXC asks `IsSupportedAsync` from a PPL task
  when it starts; a failed activation there is an exception no one
  observes, which ends the program.  The operation completes at once with
  `false`, so quick unlock is simply not offered.  `RoOriginateLanguageException`
  exists (C++/WinRT reports errors through it before throwing).
- **Tests**: `qttest` (14 checks: glyph metrics by character and by glyph
  index, gray bitmaps, outlines, C++/WinRT-style reference and heap
  HSTRINGs, Windows Hello activation and its completed operation, the
  process DACL), and KeePassXC in the nightly corpus
  (`tests/appcorpus/870-keepassxc.py`): it starts on a small KDBX 4
  database the corpus writes beside it (master password `novaos`), the
  password is typed into its unlock screen, and the unlocked database's
  screenshot must match `tests/reference/keepassxc.png`.

Next in Phase 20.3: Krita.
