# README screenshots

The images in README's "Screenshots" section.  Each is a QEMU `screendump` of
NovaOS at 2560×1600 (200% scale), resized to 1280×800 with Pillow (Lanczos)
and saved as an optimized PNG.

| File | How it was taken |
|---|---|
| `desktop.png` | `tools/novarun.py` boot; `sysinfo` in the Terminal, File Explorer opened from the dock on Projects, the two windows snapped with Win+Left and Win+Right |
| `notepad++.png` | Notepad++ 8.8.3 portable (the app corpus's download) staged with `--put DIR=C:\Apps\npp`, opened on `kernel/ke/scheduler.c` staged in `C:\Projects\src` |
| `app-store.png` | the App Store opened from the dock, Terminal minimized |
| `firefox.png`, `vlc.png`, `keepassxc.png`, `inkscape.png` | copies of `tests/reference/` screenshots, which `tools/appcorpus.py` takes of those apps |
| `teeworlds.png` | a copy of `tests/reference/teeworlds.png`: the corpus installs Mesa 3D from the App Store, starts Teeworlds in full screen and answers its first-start questions with Enter |
| `opentyrian.png` | OpenTyrian in full screen playing Tyrian's first level, drawn with Direct3D 9 through DXVK: the corpus test's steps (`tests/appcorpus/930-opentyrian.py`), then Enter on "Play Next Level" and on Tyrian, with Space held to fire |
| `blobby-volley-2.png` | the corpus test's full-screen screenshot (`tests/appcorpus/935-blobby-volley.py`) at the display's 800x600, reduced to 256 colours |
| `lbreakout2.png` | LBreakout2's first level in full screen at the display's 640x480 (`tests/appcorpus/940-lbreakout2.py`'s steps, then Local Game, Start Original Set), reduced to 256 colours |
| `beneath-a-steel-sky.png` | a copy of `tests/reference/beneath a steel sky.png`: the corpus installs ScummVM, starts the game, skips the intro and walks Foster with a click |

To refresh one, take a new screenshot the same way (`!shot NAME.png` in
`tools/novarun.py`, or `tools/appcorpus.py --only NAME --update-reference`
for the corpus apps), resize it to 1280×800 and replace the file here.
