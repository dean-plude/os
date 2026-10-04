/*
 * update_key.h — the key NovaOS's update channel is signed with
 *
 * The public half of the Ed25519 key pair whose secret half signs the
 * channel file of each release (the GitHub Actions secret
 * NOVAOS_UPDATE_SIGNING_KEY; docs/releasing.md says how it is made).  It
 * is 64 hex digits.  With a key here the updater installs only from a
 * channel file signed with it; while it is "" no key is built in, and a
 * channel is used without its signature being checked (the log says so).
 */

#pragma once

#define UPDATE_SIGNING_KEY ""
