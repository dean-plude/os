# Updating NovaOS (Phase 22.2)

An installed NovaOS updates itself from an **update channel**: a small
text file on a web server (by default, the newest GitHub release's
`novaos-update.txt`) that names the newest version and its files.  The
App Store's **Updates** page checks it, downloads a newer version and
installs it; the next restart starts the new version.  If the new
version does not start, NovaOS goes back to the one it had.

NovaOS running live from its installation disc or USB stick has nothing
to update: a newer build there is a newer ISO.

## Using it

Open the App Store and choose **Updates**.  The page checks the channel
the first time it is shown, and says either that this NovaOS is up to
date or which version is available, with its size and a line of notes.
**Update** downloads it, checks it and writes it next to the installed
system; **Restart** then starts it.  The page also says where updates
come from, and after an update (or one that did not start) what
happened.

The Terminal does the same:

```
C:\Documents> update
This is NovaOS 0.1.0. Checking https://github.com/dean-plude/os/releases/latest/download/novaos-update.txt ...
NovaOS 0.1.1 is available (38.9 MB): Fixes and new drivers.
Type 'update install' to install it.
C:\Documents> update install
...
NovaOS 0.1.1 is ready: restart to finish the update (shutdown /r).
```

`update channel` shows the channel, `update channel <url>` changes it
(kept in the registry: `HKLM\SOFTWARE\NovaOS\Update`, value `Channel`)
and `update channel default` goes back to the GitHub release.
`store updates` opens the App Store on its Updates page.

## How an update replaces a running system

An installed NovaOS is two files on its EFI System Partition (ESP): the
boot loader, `\EFI\BOOT\BOOTX64.EFI`, and the kernel,
`\EFI\NOVA\kernel.elf`.  The kernel carries every system DLL and
program; they are unpacked onto drive C: at each start and never saved
to disk, so replacing the kernel replaces all of them at once.  Both
files are in use while NovaOS runs, so the update is a pending rename,
done at the next start (the same idea as `MoveFileEx`'s
`MOVEFILE_DELAY_UNTIL_REBOOT`, Phase 17.5, one level down: the boot
loader and the new kernel carry it out, since the files are below
drive C:).

1. **Download and check** (`kernel/fs/update.c`).  The files are
   downloaded over HTTPS; each one's size and SHA-256 must be the ones
   the channel names, the kernel must be an ELF file and its stamped
   version (below) the channel's version, and the boot loader an EFI
   program.
2. **Stage.**  Earlier leftovers (and the previous update's
   `kernel.old`) are deleted to make room, then the files are written as
   `\EFI\NOVA\kernel.new` and `\EFI\NOVA\bootx64.new` and read back,
   `\EFI\NOVA\update.txt` notes the versions, and last of all the mark
   `\EFI\NOVA\update.pnd` ("pending") says the update is ready.  Until
   the mark is on the disk, nothing the running system or the boot
   loader uses has changed.  The ESP is 128 MiB; the kernel is about
   40 MB, and an update needs room for two.
3. **First start** (`bootloader/src/main.c`).  The boot loader finds the
   pending mark, makes sure `kernel.new` begins as an x86-64 ELF kernel
   would, writes the mark `update.try` ("trying"), deletes the pending
   one, and starts `kernel.new`, telling it so (`BOOT_FLAG_UPDATE_TRIAL`).
4. **Finish.**  Once the new kernel has reached the desktop, it renames
   `kernel.elf` to `kernel.old` and `kernel.new` to `kernel.elf`,
   replaces `BOOTX64.EFI` with the new boot loader, and deletes the
   trying mark.  The log says
   `[UPDATE] Updated NovaOS from 0.1.0 to 0.1.1 (the previous kernel is kept as \EFI\NOVA\kernel.old)`.

**Going back.**  If the new kernel stops before it gets that far, or the
PC is reset or loses power while it starts, the trying mark is still
there at the next start.  The boot loader then deletes it and starts the
old `kernel.elf`, telling it the update failed
(`BOOT_FLAG_UPDATE_FAILED`), and the old NovaOS deletes the update's
files and says so on the Updates page.  The order of each step is chosen
so that a power cut anywhere leaves a system that starts: the pending
mark comes after the files, the trying mark before the pending one goes,
and if the power goes between the two renames, the boot loader starts
whichever kernel is there (`kernel.new`, else `kernel.old`).

The renames are `FatRename` (`kernel/fs/fat.c`, new with updates): it
writes the new directory entry before it erases the old one, so a crash
in between leaves both names, not neither.

## The version stamp

The version (`NOVA_VERSION` in `kernel/ke/version.h`) is kept in a
marked field of the kernel image (`kernel/ke/version.c`), and what the
banner, Settings > About, `ver` and the updater show is read from that
field.  `tools/mkupdate.py --version` writes another version into it on
a copy of a built kernel, so a test update to a newer version needs no
second build; the updater also reads a downloaded kernel's field to
check it is the version the channel says.

Versions compare number by number (`0.1.10` is newer than `0.1.9`), and
a version with a suffix (`0.1.1-test`, `0.1.1-rc1`) is older than the
same version without it.  A development build's `+dev.` stamp
(`0.1.0+dev.20261004125600`, below) reads as a fourth number, so it is
newer than `0.1.0` and older than `0.1.1`.

## The channel file

```
NovaOS update 1
version 0.1.1
kernel kernel.elf 40732656 6b1f...e09c
loader bootx64.efi 82821 0d4a...71f2
notes Fixes and new drivers.
```

The first line names the format.  `kernel` and `loader` give a file's
name (relative to the channel file's address, or a full URL), its size
in bytes and its SHA-256; `loader` may be left out.  Lines NovaOS does
not know are skipped, so later versions of the format can add some.

`tools/mkupdate.py` writes the three files from a build:

```bash
python3 tools/mkupdate.py update-out --notes "Fixes and new drivers."
```

makes `update-out/kernel.elf`, `update-out/bootx64.efi` and
`update-out/novaos-update.txt`.  Attached to a GitHub release, they
are what the default channel,
`https://github.com/dean-plude/os/releases/latest/download/novaos-update.txt`,
finds: GitHub sends that address to the newest release's file, and the
files named in it are fetched from the same place.  A release made from a
version tag carries them: `.github/workflows/release.yml` runs
`tools/mkupdate.py` on the build its CI tested ([releasing.md](releasing.md)).
Pre-releases (a version like `0.1.1-rc1`) and the `latest` build of
`main` never carry the "Latest" mark once a release exists, so installed
systems only see releases.

## The rolling build's channel

Each green run of the CI on `main` moves the `latest` pre-release to that
commit (`.github/workflows/ci.yml`, job `publish-iso`), and attaches an
update channel of its own next to `nova.iso`: the kernel and boot loader
that run tested, made with the same `tools/mkupdate.py` as a release.  Its
kernel is stamped with a development version, the version it was built
with followed by `+dev.` and the commit's time (UTC, `YYYYMMDDHHMMSS`):

```
NovaOS update 1
version 0.1.0+dev.20261004125600
kernel kernel.elf 40732656 6b1f...e09c
loader bootx64.efi 82821 0d4a...71f2
notes NovaOS 0.1.0+dev.20261004125600, a development build of main at 108849e: ...
```

That version is newer than the release it follows (`0.1.0`) and than
every earlier development build, and older than the next release
(`0.1.1`).  Every NovaOS since 0.1.0 orders it so, since the comparison
reads the time as a fourth number; nothing new is needed on an installed
system.  To follow `main`:

```
update channel https://github.com/dean-plude/os/releases/download/latest/novaos-update.txt
```

and `update channel default` goes back to releases.  A system on the
default channel never sees these builds: the default address follows
GitHub's "Latest" mark, which only tagged releases carry.  A system
that went back to releases from a development build stays on it until
the next release, which is newer.  Two builds of `main` within the same
second would carry the same version; the second reads as up to date.
The ISO on `latest` keeps the plain version, so a system installed from
it is offered the same build once more on this channel.

## The self-test

The devices suite's `update` boot (`tests/selftest/devices/update`)
starts `build/nova.img` as an installed NovaOS on QEMU's user-mode
network, where `tools/selftest.py` serves channels made with
`tools/mkupdate.py` from the same build: `v1/`, stamped one version
newer than the build (`0.1.1-test` for 0.1.0), and `v2/`, two newer.

- `update` finds the v1 version, `update install` stages it, and a
  restart starts it and finishes the update (`ver` says the new
  version);
- a second restart starts it again, and the channel says it is up to
  date;
- v2 is staged, and the machine is reset as soon as its kernel says it
  is starting: the next start is the v1 version again, which removes the
  update and offers it again;
- `store updates` shows the App Store's Updates page (its screenshot is
  `store-updates.png`).

Before that it checks the version order of the rolling build's channel
with two more channels: `same/`, stamped with the build's own version, is
up to date, and `dev/`, stamped as a development build of it
(`0.1.0+dev.20261004125600`), is offered; once the v1 version is
installed, `dev/` is up to date (older).

```bash
python3 tools/selftest.py --suite devices --only 'same channel,same version up to date,dev channel,dev build newer,update channel,update check,update install,restart into it,updated,restart again,up to date,dev channel again,dev build older,next update,stage it,reset while trying,offered again,store updates'
```

## Not done yet

- **Signatures.**  An update is trusted because it comes over HTTPS from
  the channel's server and matches the SHA-256 the channel file names.
  A signed channel file (a key pair whose public half is built into
  NovaOS) would also cover a changed release page.
- **Updates on the reference laptop** are checked like the rest of
  Phase 21, by hand ([install-and-power.md](install-and-power.md)): an
  installed T14 updating from a release.
