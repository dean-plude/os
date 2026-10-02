## Regression gate: boot CI on every pull request

- **GitHub Actions** (`.github/workflows/ci.yml`): every pull request and
  every push to main builds the kernel, bootloader, userland and
  `build/nova.img` on Ubuntu 24.04, boots it in QEMU (q35, OVMF, TCG) and
  runs the self-tests.  A failing test fails the "Build and boot-test"
  check; the step summary has a table of results and the serial log,
  screenshots and sound recording are kept as an artifact.
- **`tools/selftest.py`**: one boot, then each test typed into the
  Terminal.  A test passes on exit code 0, no `FAIL` line, no non-zero
  "failed" count and the output it expects; a kernel panic ends the run.
  The boot has an HD Audio card recorded to a WAV, which must hold the
  tones `soundtest` played, and the battery in `tests/acpi/battery.asl`
  (75%, 3 h left), which `battery` must report.
- **`guitest auto`** drives its own menus, edit and list boxes, the
  resource dialog, a message box and the property sheet, and reports.
- `tools/novarun.py`'s boot-and-type logic is now a `Nova` class that
  other tools import.
