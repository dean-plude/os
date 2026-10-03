## First-boot setup: Welcome to NovaOS (Phase 22.1, name and display)

An installed NovaOS used to start straight onto a desktop that belonged to
"Dean Plude".  The first time it now starts from the disk Setup put it on,
**Welcome to NovaOS** (`kernel/apps/welcome.c`) opens before anything else
and asks, with no Terminal commands:

- **Your name.**  Up to 20 characters, without the characters Windows
  refuses in a user name.  It goes to `HKLM\SOFTWARE\NovaOS\Setup`
  (`UserName`) and to `RegisteredOwner`; programs started from then on
  get it as `USERNAME`, so `GetUserName` returns it, and the Start menu
  shows it with its initials.
- **Display.**  The modes the display offers, the current one marked; a
  click or the arrow keys switch to a mode at once and keep it across
  restarts, as Settings does.
- **Finish.**  `FirstBootDone` is set and the desktop takes over.

It opens by itself only when NovaOS did not start from the installation
media and drive C: is kept on a disk with Setup's two partitions
(`NOVA_EFI` and `NOVADATA`), so the QEMU test images and live sessions are
unchanged.  `start welcome` opens it on any system.

`whoami.exe` (System32) prints `nova-pc\name` from `GetUserName`, and the
Terminal's own `whoami` gives the same answer.  The core self-test
`welcome` answers the screens from the keyboard (a name, a resolution
tried and put back); `whoami` and `whoami builtin` then check the name.
Checked by hand in QEMU: installed from `nova.iso` onto an NVMe disk, the
first start from that disk opened the setup, and the second did not.

Time zone and keyboard layout pages are the rest of step 22.1: NovaOS
still keeps UTC and the US layout.
