# Releasing NovaOS

A release is made by pushing a version tag.  The tag starts
`.github/workflows/release.yml`, which builds and tests the tagged commit
exactly as every pull request is tested, and only then publishes the
GitHub release.  Nothing is uploaded by hand.

## Cutting a release

1. On `main`, `NOVA_VERSION` in `kernel/ke/version.h` is the version to
   release (0.1.0 for the first), and `docs/releases/VERSION.md` holds
   the hand-written part of its notes: what the release is, how to start
   it, its known limits.  Both change through a pull request like any
   other file.
2. Tag the merged commit and push the tag:

   ```bash
   git fetch origin main
   git tag -a v0.1.0 origin/main -m "NovaOS 0.1.0"
   git push origin v0.1.0
   ```

   On GitHub's web page the same is **Releases**, **Draft a new release**,
   **Choose a tag**, type `v0.1.0`, **Create new tag on publish** with
   target `main`, no description, **Set as the latest release** unticked,
   **Publish release** (this works from a phone's browser).  That
   publishes an empty release at once, which the workflow then fills and
   marks Latest; a "pre-release" tick is cleared for a plain version.
3. Watch the **Release** run on the Actions page.  It takes as long as a
   pull request's CI (the graphics suite is the longest part).

## What the workflow does

- **Checks the tag.**  The tag must be `v` followed by `NOVA_VERSION`
  (`v0.1.0` for "0.1.0"), and `docs/releases/0.1.0.md` must exist;
  otherwise the run stops before building anything.
- **Runs CI on the tag** (`ci.yml`, called with `release: true`): the
  checks, the boot-test job's core, network and devices suites (the
  devices suite also starts NovaOS from the ISO) and the graphics suite.
  The boot-test job first runs `tools/fetch_sof_firmware.py --all`, so the
  image carries the Sound Open Firmware the audio DSP needs for laptop
  microphones (the T14 Gen 4 and other Tiger Lake to Raptor Lake
  machines); pull requests build without it.  It keeps the ISO, the
  kernel and the boot loader it tested.
- **Publishes the release** for the tag, titled "NovaOS 0.1.0", with:

  | File | What it is |
  |---|---|
  | `nova.iso` | the bootable ISO (CD and USB stick image, live system and installer) |
  | `nova.iso.sha256` | its SHA-256, for `sha256sum -c` |
  | `kernel.elf`, `bootx64.efi`, `novaos-update.txt` | the update channel, made by `tools/mkupdate.py` from the same kernel and boot loader ([updates.md](updates.md)) |
  | `SHA256SUMS` | the SHA-256 of all of them |

  `novaos-update.txt` is signed with the secret `NOVAOS_UPDATE_SIGNING_KEY`
  ([below](#the-update-signing-key)).  Without the secret it is published
  unsigned, with a warning on the run; with a secret whose public half is
  not the one `kernel/fs/update_key.h` builds in, or with that key set and
  no secret, the run stops before publishing.

  The notes are made by `tools/release_notes.py`: `docs/releases/VERSION.md`,
  a table of the files with their sizes and SHA-256, and a list of every
  `docs/history/` section added since the previous release tag (every
  section, for the first release), each linking to the tagged file.
- **Marks it "Latest".**  Installed NovaOS systems read
  `releases/latest/download/novaos-update.txt`, so this is what offers the
  release to them as an update.  A version with a suffix (`v0.1.1-rc1`)
  becomes a pre-release instead and is offered to nobody.  The CI's
  rolling `latest` build of `main` stops claiming the mark once the first
  release exists.

A failed run publishes nothing, since publishing is its last step; fix
the cause on `main` and tag again.  Re-running a run from the Actions page
replaces the files and notes of a release already there.  A tag on the
wrong commit is removed with `git push origin :refs/tags/v0.1.0` (and its
release, if one was made, deleted on the release page) before tagging
again.

## The update signing key

Installed NovaOS systems install only from a channel file signed with the
key built into them ([updates.md](updates.md#signatures)).  The key pair
is made once, by the owner of the repository, on a computer they trust;
its secret half never goes into the repository.

1. Make the key pair from a checkout of the repository.  It writes the
   secret into the file and prints the public key (64 hex digits):

   ```bash
   python3 tools/mkupdate.py --new-key ~/novaos-update.key
   ```

2. Store the secret as the repository's Actions secret (or on GitHub:
   **Settings**, **Secrets and variables**, **Actions**, **New repository
   secret**, name `NOVAOS_UPDATE_SIGNING_KEY`, the file's one line as the
   value):

   ```bash
   gh secret set NOVAOS_UPDATE_SIGNING_KEY --repo dean-plude/os < ~/novaos-update.key
   ```

3. Put the printed public key into `kernel/fs/update_key.h`
   (`#define UPDATE_SIGNING_KEY "8cafb488..."`) through a pull request.
   `python3 tools/mkupdate.py --public-key ~/novaos-update.key` prints it
   again.
4. Keep `~/novaos-update.key` somewhere safe and offline as well: systems
   built with the key take updates only from channels it signs, so a lost
   key means their next version must be installed from a new ISO.

The next release then publishes a signed channel.  Releases made before
step 3 merged keep working: their kernels have no key and check nothing.

## Trying it before tagging

The same steps run locally after a build:

```bash
python3 tools/fetch_sof_firmware.py --all
cmake -S . -B build -G "Unix Makefiles"
make -C build -j"$(nproc)"
scripts/create-iso.sh dist/nova.iso build/bootx64.efi build/kernel.elf
python3 tools/mkupdate.py dist --notes "NovaOS 0.1.0" --sign ~/novaos-update.key
python3 tools/release_notes.py 0.1.0 > notes.md
```

`tools/release_notes.py` prints the files table too when given
`--dist dist` with `nova.iso.sha256` and `SHA256SUMS` written beside the
other files (`sha256sum nova.iso > nova.iso.sha256`, then
`sha256sum nova.iso kernel.elf bootx64.efi novaos-update.txt > SHA256SUMS`
inside `dist`).

## After the release

Step 22.5 of the roadmap is done when the release page is live and its
ISO boots in QEMU (the release run's devices suite does that) and on the
reference PC, a ThinkPad T14 Gen 4: started from a USB stick it reaches
the desktop, and the checks in [install-and-power.md](install-and-power.md#checks-on-the-t14)
pass.
