- ~~Sound and the touchpad on the reference laptop~~ Done (Phase 21.4): the
  HD Audio driver takes Intel's controllers with the audio DSP on (class
  04.01, as on the ThinkPad T14 Gen 4) as well as off, sets up Realtek's
  ALC256 family and turns the speakers off while headphones are plugged
  in; I2C-HID touchpads, found through ACPI on Intel's LPSS I2C
  controllers, run in their touchpad mode with tap to click, two-finger
  tap for the right button and two-finger scrolling (a follow-up PR).  Checked
  in QEMU against modelled devices (`hwcheck` in the core suite, with an
  ACPI table describing a touchpad); on the T14 the check is by hand
  ([hardware.md](hardware.md)).  Not yet: the digital microphones (they
  sit behind the DSP), the touchpad's interrupt line (polled for now),
  tap-and-drag and scrolling that coasts on after the fingers lift.
