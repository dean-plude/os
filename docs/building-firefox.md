# Bundled Firefox

Every normal build includes Firefox 157.0 (Windows x64, en-US), matching the
App Store regression version. Install `p7zip-full` (or `7zip`) alongside the
normal toolchain. The build retrieves the installer and SHA512SUMS from
Mozilla's versioned HTTPS archive, verifies SHA-512, extracts the installer
with 7z and embeds every file in its core directory. It rejects empty,
corrupt or incomplete downloads and symlinks in the payload. Existing valid
installer bytes are reused; a failed refresh does not overwrite them.

The cache defaults to `~/.cache/novaos/firefox`; set `NOVA_FIREFOX_CACHE` to
change it. The build's `userland/firefox/provenance.json` records version,
source URL and SHA-512. Updating Firefox requires changing the pinned version
alongside the Store and corpus fixture and validating compatibility again.
The unmodified payload retains Mozilla's notices; public distributors should
follow Mozilla's distribution policy at
https://www.mozilla.org/en-US/foundation/trademarks/distribution-policy/.

The default browser is selected by the NovaOS shell, rather than modifying
Firefox's preferences or pretending its Windows installer ran. No test CA or
corpus policy is shipped. `browser` opens Firefox from the desktop launcher;
NetSurf can still be launched explicitly. Full Firefox functionality remains
subject to NovaOS application compatibility testing.

The disk builder sizes the EFI image for the installed kernel and loader plus
one similarly sized staged replacement, with another 32 MiB for FAT metadata,
update markers and persisted configuration. It rounds up to 32 MiB boundaries
and retains the 128 MiB minimum. This headroom is required because bundled
applications live inside the kernel image and updates keep the current image
until a trial boot succeeds. The updater still rejects insufficient space.
