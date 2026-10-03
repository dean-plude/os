## Releases from a version tag (Phase 22.5)

NovaOS releases are now made by pushing a version tag
([releasing.md](releasing.md)).  `.github/workflows/release.yml` checks
that the tag names the kernel's version (`v0.1.0` for 0.1.0) and that
release notes exist, runs the whole CI workflow on the tagged commit, and
only when every suite passes publishes the GitHub release with the ISO
that was tested.

- **The release build** fetches the Sound Open Firmware for the audio DSP
  first (`tools/fetch_sof_firmware.py --all`), so laptop microphones work
  in the shipped image; pull requests still build without it.
  `ci.yml` can be called from another workflow (`workflow_call`, input
  `release`) and then also keeps the kernel and boot loader it built.
- **The release's files**: `nova.iso` and `nova.iso.sha256`, the update
  channel (`kernel.elf`, `bootx64.efi`, `novaos-update.txt` from
  `tools/mkupdate.py`) that installed systems read from the newest
  release, and `SHA256SUMS`.
- **Release notes** from `tools/release_notes.py`: the hand-written
  `docs/releases/VERSION.md`, the files with their sizes and SHA-256, and
  every history section added since the previous release.
- **"Latest" belongs to releases.**  Once a release exists, CI's rolling
  `latest` build of `main` is a pre-release, so the README's download
  link and the update channel follow releases; versions with a suffix
  (`0.1.1-rc1`) are published as pre-releases.
