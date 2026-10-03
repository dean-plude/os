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

To refresh one, take a new screenshot the same way (`!shot NAME.png` in
`tools/novarun.py`, or `tools/appcorpus.py --only NAME --update-reference`
for the corpus apps), resize it to 1280×800 and replace the file here.
