## Release documentation (Phase 22.4)

The documentation NovaOS 0.1 ships with, written for people who did not
build it:

- **A user guide**, [docs/user-guide.md](user-guide.md): getting the ISO,
  trying it in QEMU, installing it, the desktop and its keyboard
  shortcuts, the built-in apps, installing programs from the App Store and
  elsewhere, where files are kept, network, sound, power, and what to do
  when something fails.  It says plainly that 0.1 has only been checked in
  QEMU.
- **A compatibility list**, [docs/compatibility.md](compatibility.md):
  every program known to run, the App Store's untested ones, a status
  (Works, Partly, Untested) and how each was checked (the nightly corpus,
  the CI graphics tests, the core self-tests, or by hand).  Each row is a
  file in `docs/compatibility/`, and `tools/docgen.py` builds the table, so
  a change that makes a program work edits that program's row.  Step 20.7
  will have CI keep the statuses.
- **Where NovaOS runs**, at the top of [docs/hardware.md](hardware.md):
  QEMU (tested on every pull request), other virtual machines (untested),
  real PCs (none checked yet) and Macs.
- **The build guide checked from a clean clone.**  Following
  `docs/building.md` from the top on Ubuntu 24.04 found two faults: its
  package list left out MinGW-w64, without which `msvcp140.dll` does not
  build, and the README's shorter list also left out `libc++-dev`; and
  the Ninja build it recommends stopped at once, because CMake did not
  know the bootloader sub-build makes `bootx64.efi` (CI builds with
  Makefiles, which do not mind).  Both lists now name every package,
  `CMakeLists.txt` declares the bootloader's output, and the same steps
  build `nova.img` in about three minutes on four cores and boot it to the
  desktop with `cmake --build . --target run`.  The macOS guide's Linux
  container command got the same package, and its UTM settings now allow
  an NVMe disk, which NovaOS drives since Phase 18.3.
