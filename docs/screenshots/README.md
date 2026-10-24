# Screenshots

The images in the README's "Screenshots" section, and how each was made.
All are of NovaOS running in QEMU, saved as optimized PNG files.

- **Desktop and app windows** are QEMU `screendump`s of NovaOS at
  2560x1600 and 200% scale, resized to 1280x800 with Pillow (Lanczos).
- **Full-screen games** that switch the display to a lower resolution are
  kept at that resolution (640x480 or 800x600) and reduced to 256
  colours.

| File | Size | How it was taken |
|---|---|---|
| `desktop.png` | 1280x800 | The `latest` ISO of 2026-10-14 booted live in QEMU (q35, 4 CPUs, 4 GB, standard VGA at 2560x1600), the installer closed with Esc: the default Aurora desktop with nothing open.  Taken with QMP `screendump`. |
| `install.png` | 1280x800 | The same boot before Esc: **Install NovaOS**, which a live start opens by itself. |
| `notepad++.png` | 1280x800 | Notepad++ 8.8.3 portable (the app corpus's download) staged with `--put DIR=C:\Apps\npp`, opened on `kernel/ke/scheduler.c` staged in `C:\Projects\src`. |
| `app-store.png` | 1280x800 | The App Store opened from the dock, with the Terminal minimized. |
| `task-manager.png`, `task-manager-performance.png` | 1280x800 | Task Manager opened with Ctrl+Shift+Esc over `winhello`, on Processes with `winhello.exe` selected and then on Performance: the screenshots `tests/selftest/core/166-taskmgr.py` takes (`tools/selftest.py --suite core --only taskmgr`), QEMU with 2 CPUs. |
| `calculator.png`, `calculator-scientific.png`, `calculator-programmer.png` | 1280x800 (taken at 2560x1600) | Calculator started from the Start menu: Standard after 1 ÷ 7, Scientific after 25!, Programmer with 255 in hexadecimal: the screenshots `tests/selftest/core/168-calc.py` takes (`tools/selftest.py --suite core --only calc`), scaled down. |
| `firefox.png`, `vlc.png`, `keepassxc.png`, `inkscape.png` | 1280x800 | Copies of the `tests/reference/` screenshots that `tools/appcorpus.py` takes of those programs. |
| `teeworlds.png` | 1280x800 | A copy of `tests/reference/teeworlds.png`: the corpus starts Teeworlds in full screen on the built-in OpenGL and answers its first-start questions with Enter. |
| `beneath-a-steel-sky.png` | 1280x800 | A copy of `tests/reference/beneath a steel sky.png`: the corpus installs ScummVM, starts the game, skips the intro and walks Foster with a click. |
| `opentyrian.png` | 1280x800 | OpenTyrian in full screen playing Tyrian's first level, drawn with Direct3D 9 through DXVK: the steps of `tests/appcorpus/930-opentyrian.py`, then Enter on "Play Next Level" and on Tyrian, with Space held to fire. |
| `blobby-volley-2.png` | 800x600 | The full-screen screenshot of `tests/appcorpus/935-blobby-volley.py`, at the display's 800x600, reduced to 256 colours. |
| `lbreakout2.png` | 640x480 | LBreakout2's first level in full screen: the steps of `tests/appcorpus/940-lbreakout2.py`, then Local Game, Start Original Set; reduced to 256 colours. |
| `cave-story.png` | 640x480 | Cave Story's First Cave in full screen: the steps of `tests/appcorpus/945-cave-story.py`, then Z held through the opening lines; reduced to 256 colours. |
| `device-manager.png` | 1280x800 | A copy of the devices suite's "devmgr" screenshot (`tests/selftest/devices/devmgr/010-devmgr.py`): `devmgmt --check` with every class open and the display adapter selected, resized from 2560x1600. |
| `event-viewer.png` | 1280x800 | The "event viewer start menu" core self-test's screenshot (`tests/selftest/core/167-eventlog.py`): `eventvwr --check` with `evtlogtest`'s event selected in the Application log, resized from 2560x1600. |
| `supertux.png` | 640x480 | SuperTux's first level, "Welcome to Antarctica", in full screen, with Tux walked right using the game pad (the steps of `tests/appcorpus/947-supertux.py`); reduced to 256 colours. |
| `disk-management.png` | 1280x800 | The devices suite's `diskmgr` boot (`tests/selftest/devices/diskmgr`): Disk Management after `diskmgmt --test` has made an NTFS and a FAT32 volume on the blank 256 MB Disk 2 and moved the FAT32 one to T:, resized from the test's 2560x1600 screendump. |

`camera.png` (1280x800) is not in the README either.  It is the devices
suite's `camera` boot's screenshot of the Camera app showing the fake UVC
camera's test card (`tests/selftest/devices/camera/010-camera.py`, resized
to 1280x800), the reference picture for roadmap step 28.3.

`aurora-preview.png` (1672x941) is not in the README.  It is a preview of
the Aurora desktop rendered on a Linux host with NovaOS's own drawing code
(see [history/2026-10-05-aurora-desktop.md](../history/2026-10-05-aurora-desktop.md)),
not a screenshot of NovaOS running, and
[design/native-desktop.md](../design/native-desktop.md) refers to it.

## More screenshots

These are not in the README's table; the [user guide](../user-guide.md)
shows some of them.  The **fresh** ones come from the `latest` ISO of
2026-10-14 booted live in QEMU (q35, 4 CPUs, standard VGA at 2560x1600,
e1000e network card, USB tablet) with the installer closed by Esc, driven
over QMP (`send-key`, `input-send-event`, `screendump`).  Shots with the
Aurora wallpaper are reduced to 256 colours to keep them small.  The
**earlier** ones were taken by the threads that built each feature, on the
build of the day shown in the dock's clock, most of them on the Sunset
desktop of early October.

### The desktop and built-in apps

| File | Size | How it was taken |
|---|---|---|
| `start-menu.png` | 1280x800 | Fresh: the Windows key on the Aurora desktop: pinned apps, installed programs and recent items. |
| `file-explorer.png` | 1280x800 | Fresh: File Explorer from the dock, on `C:\Windows` with the preview pane. |
| `terminal.png` | 1280x800 | Fresh: the Terminal from the dock, running `sysinfo` and `devices`. |
| `notepad.png` | 1280x800 | Fresh: Notepad from the dock, with two lines typed. |
| `calendar.png` | 1280x800 | Fresh: Calendar from the dock, over Notepad. |
| `settings-display.png` | 1280x800 | Settings from the dock, on Display (resolution and scale). |
| `settings-system.png`, `settings-personalization.png`, `settings-personalization-scrolled.png`, `settings-time-language.png`, `settings-network.png`, `settings-search.png`, `settings-narrow.png` | 1280x800 | 2026-10-21, after the Settings page framework: System with Recovery; Personalization at its top and scrolled (the scroll bar, the content clipped under the title); Time & language; Network; **Find a setting** with "net" typed (Network, Wi-Fi and Printers found); and the window snapped to half the screen (Win+Left), where the sidebar shows only the pages' icons.  Taken under QEMU's software emulation from a local build. |
| `settings-sound.png` | 1280x800 | Earlier (2026-10-03): Settings > Sound with an Intel HD Audio card, a QEMU USB audio device and a USB headset, the headset chosen. |
| `first-start-setup.png` | 1280x800 | Earlier (2026-10-03): **Welcome to NovaOS** on its time zone page, with "Berlin" typed. |
| `app-store-badges.png` | 1280x800 | Earlier (2026-10-14, roadmap step 27.5): the App Store's catalog with its Verified and Platinum badges, over the Aurora home panel. |
| `complex-scripts.png` | 640x200 | Earlier (2026-10-02): NovaOS's text drawing of Arabic and Devanagari next to Latin text. |

### Programs

| File | Size | How it was taken |
|---|---|---|
| `winscp.png`, `thunderbird.png`, `audacity.png`, `sumatrapdf.png`, `winmerge.png`, `putty.png`, `openttd.png`, `photocraft.png`, `vectorcraft.png`, `effectcraft.png` | 1280x800 | Copies of the `tests/reference/` screenshots that `tools/appcorpus.py` takes of those programs (`tests/appcorpus/952-winscp.py`, `954-thunderbird.py`, `880-audacity.py`, `850-sumatrapdf.py`, `860-winmerge.py`, `950-putty.py`, `910-gog-openttd.py`, `886-photocraft.py`, `887-vectorcraft.py`, `888-effectcraft.py`). |
| `winui3-first-page.png` | 961x543 | Roadmap step 32.4: the first frame of a WinUI 3 page (a `Border`, a `TextBlock` and a `Button` in a `Window`), the core self-test `xamlpagetest`'s screenshot (`tests/selftest/core/102-xamlpagetest.py`, `tools/selftest.py --only xamlpagetest`) cropped to the window and halved.  In the [XAML design](../design/windows-ui-xaml.md), the roadmap and the history, not in the README. |
| `krita.png` | 1280x800 | Earlier (2026-10-14, roadmap step 27.3): Krita with a line drawn and saved as `drawing.kra`, as in `tests/appcorpus/885-krita.py`. |
| `libreoffice-calc.png` | 1280x800 | Earlier (2026-10-14, roadmap step 27.2): LibreOffice Calc with `sums.xlsx` open, as in `tests/appcorpus/953-libreoffice.py`. |
| `7zip-setup.png` | 1280x800 | Earlier (2026-10-02): the 7-Zip `.msi` installed with `msiexec /i`, on its Custom Setup feature tree. |
| `drag-and-drop.png` | 1280x800 | Earlier (2026-10-03): files dragged from 7-Zip's file manager onto an OLE drop target (`droptest ole`). |
| `neovim.png`, `msys2-shell.png`, `git-log.png` | 1280x800 | Earlier (2026-10-02): Neovim in the Terminal, MSYS2's `sh` running coreutils, and `git log` through the Terminal's pager. |
| `webview2.png` | 1280x800 | Earlier (2026-10-05): a page drawn by Microsoft Edge WebView2 in a host program. |

### Graphics and games

| File | Size | How it was taken |
|---|---|---|
| `opengl.png` | 1280x800 | Earlier (2026-10-01): `gltest`'s OpenGL triangle (`tests/selftest/graphics/100-gltest.py`). |
| `direct3d-9.png` | 1280x800 | Earlier (2026-10-01): `d3d9test`'s Direct3D 9 triangle (`tests/selftest/graphics/008-d3d9test.py`). |
| `direct2d.png` | 1280x800 | Earlier (2026-10-02): `d2dtest`'s Direct2D scene in a window (`tests/selftest/graphics/005-d2dtest.py`). |
| `squash-the-creeps.png` | 1280x800 | Earlier (2026-10-12, roadmap step 24.4): Godot's Squash the Creeps sample on Direct3D 12. |
| `dos-bootrogue.png` | 1280x800 | Roadmap step 31.4: the DOS game bootRogue (`C:\Tests\Dos\ROGUE.COM`) in the Terminal's console window on `ntvdm.exe`, after eight moves: the core self-test `dostest`'s screenshot (`tests/selftest/core/169-dos.py`, `tools/selftest.py --suite core --only dostest`), resized from 2560x1600.  In [dos.md](../dos.md) and the user guide, not in the README. |

## Refreshing a screenshot

1. Take it the same way: `!shot NAME.png` in a `tools/novarun.py`
   command, QMP `screendump` on a live boot of the ISO, or
   `tools/appcorpus.py --only NAME --update-reference` for a corpus
   program (which writes its reference screenshot in `tests/reference/`).
2. Resize a desktop screenshot to 1280x800, or reduce a full-screen game
   screenshot (or one with the Aurora wallpaper) to 256 colours.
3. Replace the file here, and update its row above if the steps changed.
