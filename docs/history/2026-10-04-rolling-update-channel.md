## An update channel on the rolling build of main

The CI's `latest` pre-release (every green run on `main`) had an ISO but
no update channel, and had it carried one, its version would have been
`0.1.0`, which an installed 0.1.0 reads as up to date.  It now carries
`kernel.elf`, `bootx64.efi` and `novaos-update.txt`, made by
`tools/mkupdate.py` from the kernel and boot loader the run tested, the
kernel stamped `0.1.0+dev.` and the commit's UTC time
(`0.1.0+dev.20261004125600`).  NovaOS's version comparison already reads
the stamp as a fourth number, so every installed 0.1.0 sees a newer build
of `main` as an update, and the next release as newer again; the comment
in `kernel/ke/version.c` now says so.  `tools/mkupdate.py --version`
accepts the `+` part.

A system follows `main` with `update channel
https://github.com/dean-plude/os/releases/download/latest/novaos-update.txt`;
the default channel still follows GitHub's "Latest" mark, which only
tagged releases carry ([updates.md](../updates.md#the-rolling-builds-channel)).

The devices suite's `update` boot checks the order first: a channel of the
build's own version is up to date, a `+dev.` build of it is offered, and
after the update to `0.1.1-test` the `+dev.` build is older.
