- ~~Sound and the touchpad on the reference laptop~~ Done (Phase 21.4): the
  HD Audio driver takes Intel's controllers with the audio DSP on (class
  04.01, as on the ThinkPad T14 Gen 4) as well as off, sets up Realtek's
  ALC256 family and turns the speakers off while headphones are plugged
  in; I2C-HID touchpads, found through ACPI on Intel's LPSS I2C
  controllers, run in their touchpad mode with tap to click, two-finger
  tap for the right button and two-finger scrolling (a follow-up PR).  Checked
  in QEMU against modelled devices (`hwcheck` in the core suite, with an
  ACPI table describing a touchpad); on the T14 the check is by hand
  ([hardware.md](hardware.md)).  ~~The digital microphones, behind the
  DSP~~ Done (Phase 21.4): Sound Open Firmware boots on the DSP, records
  them through an IPC4 capture pipeline and they are the "Microphone
  Array (DSP)" recording device, booted again after sleep (checked on a
  modelled DSP, `hwcheck mic`).  ~~The touchpad's interrupt line~~ Done
  (Phase 21.4): a driver for Intel's GPIO controllers (Tiger Lake to
  Meteor Lake) takes the touchpad's GpioInt pin and the touchpad is read
  when it fires, polled only when no interrupt can be had (checked on a
  modelled controller, `hwcheck`).  Not yet: tap-and-drag and scrolling
  that coasts on after the fingers lift.
