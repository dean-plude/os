## VLC's picture, Teeworlds' fixture and room for more handles

The first scheduled nightly on the rebuilt CI (run 37607035777, main
3f14452) passed 28 of its 31 programs. The three failures had three
different causes.

- **VLC showed only a strip of its video**: a windowed player's
  DirectDraw primary surface is copied into the video window with
  `StretchDIBits` (`nova_present_windowed_primary` in
  `third_party/cnc-ddraw/src/ddsurface.c`). `StretchDIBits` counts the
  source rectangle's y from the bottom of the image, top-down DIBs
  included, as Windows does (NovaOS's `gdi32` follows it). The present
  passed the window's screen y as if it counted from the top, so the
  rows it copied were the ones further down the desktop-sized surface:
  only the bottom of the picture reached the top of the window and the
  rest stayed dark. It now passes the y counted from the bottom, and
  `tests/tools/test_windowed_media.py` checks the rows it asks for.
- **Teeworlds' fixture step "did not start"**: the step that deletes the
  night menu map (so the screenshot matches at any hour) is a Terminal
  command, not a program, but was not marked `builtin`, so the corpus
  waited for a `del.exe` that never runs. It is marked now; the step and
  the screenshot are unchanged.
- **Firefox's browser process stopped on a release assertion**: with
  Mozilla's symbols, `xul.dll+0x8dfd51` is `MessageChannel`'s
  constructor, `MOZ_RELEASE_ASSERT(mEvent, "CreateEvent failed! Nothing
  is going to work!")`. Round seven's run of the same crash logged
  `ERROR_TOO_MANY_OPEN_FILES` from the same process just before: its
  handle table, 4,096 handles, was full. Windows sets no such limit (a
  process may hold millions), so a process's table now holds 16,384
  (`UM_MAX_HANDLES`). A local run (TCG) counted the browser process's
  handles as it loaded the page: they level off at about 650 and go
  back down as content processes end, so no slow leak showed there.
  To find out what the nightly's run holds, a process's handles are now
  listed by kind in the log, once, when it first holds 4,096 of them and
  again if its table fills (`handle_census` in `kernel/um/um_syscall.c`:
  files, folders, consoles and objects by type, how many are named, and
  how many of the threads and processes they name have ended).
- **`DUPLICATE_CLOSE_SOURCE` closes the source whether or not the copy
  was made**, as Windows documents; NovaOS used to keep it open when the
  duplicate failed.
