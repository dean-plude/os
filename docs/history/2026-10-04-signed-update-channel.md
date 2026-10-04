## Signed update channel

The update channel file (`novaos-update.txt`, Phase 22.2) now carries an
Ed25519 signature, and a NovaOS with the release key built in installs
only from a channel signed with that key.  Before, an update was trusted
because it came over HTTPS and matched the SHA-256 its channel file
named, so whoever could change the release page could change the update.

- **The format**: a last line `signature ed25519 PUBLIC-KEY SIGNATURE`
  signs every byte before it.  NovaOS 0.1.0 skips lines it does not know,
  so it still reads signed channels.
- **The kernel** (`kernel/fs/update.c`) checks the signature with
  Monocypher's `crypto_ed25519_check` (`third_party/monocypher`, 4.0.2,
  BSD-2-Clause or CC0, unchanged) before it reads anything else of the
  file.  An unsigned channel, one signed with another key, and one
  changed after signing are refused, and the log and the Updates page say
  which.  The key is `UPDATE_SIGNING_KEY` in `kernel/fs/update_key.h`;
  while it is empty, channels are used unchecked, as before, with a log
  line saying so.
- **Signing**: `tools/mkupdate.py --sign KEYFILE` (or `--sign-env NAME`)
  signs the channel, `--new-key` makes a key pair and `--public-key`
  prints a key's public half, with a pure-Python Ed25519
  (`tools/ed25519.py`, after RFC 8032's reference code).  The release
  workflow signs with the Actions secret `NOVAOS_UPDATE_SIGNING_KEY`;
  without it the channel is published unsigned with a warning, and a
  secret that does not match the built-in key stops the release
  ([releasing.md](../releasing.md#the-update-signing-key)).
- **Test**: the devices suite's update boot signs its channels with a key
  pair kept only for the tests
  (`tests/selftest/devices/update/TEST-ONLY-signing-key.txt`), whose
  public half QEMU hands to NovaOS through its firmware configuration
  device (`-fw_cfg name=opt/novaos/update-key`), which only a virtual
  machine's host can set and which a PC does not have.  Three refused
  channels come first (`005-signature.py`), then the signed update of
  `010-update.py`.
