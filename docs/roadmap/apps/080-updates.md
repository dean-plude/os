- ~~Updates: an update channel in the App Store that replaces the system
  safely~~ Done (Phase 22.2): the App Store's Updates page and the
  Terminal's `update` download a newer kernel and boot loader from the
  channel (a GitHub release by default, made with `tools/mkupdate.py`),
  check them, and stage them; the boot loader starts the new kernel once
  and goes back to the old one if it does not reach the desktop
  ([updates.md](updates.md)).  The channel file is signed with Ed25519;
  a NovaOS with the release key built in installs only from a channel
  signed with it.
