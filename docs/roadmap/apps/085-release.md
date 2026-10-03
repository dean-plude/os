- Release 0.1 (Phase 22.5): pushing a version tag builds, tests and
  publishes a release (`.github/workflows/release.yml`,
  [releasing.md](releasing.md)): every CI suite on the tagged commit, with
  the audio DSP firmware fetched first, then `nova.iso`, its checksums,
  the update channel's files and notes from the history.  Still to do:
  tag `v0.1.0`, and start the release's ISO on the reference PC.
