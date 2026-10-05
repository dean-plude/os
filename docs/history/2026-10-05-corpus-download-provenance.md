## Corpus downloads have identities and a refresh policy

Downloads use the entire source URL as their cache identity, preventing equal
filenames and query URLs from colliding. Roblox, Steam and evergreen WebView2
refresh after 24 hours by default (`--mutable-max-age 0` forces refresh).
Versioned inputs keep their cache entries. Failed refreshes fail staging and
leave the previous bytes intact; downloads and metadata are atomically replaced.

Each run retains `downloads.json` with source and resolved URLs, SHA-256 hashes,
fetch times, declared versions and versions found in resolved URL paths when
available. WebView2 runtime discovery and Roblox version-directory output also
record observed versions with their evidence. Unknown resolved versions stay null; a filename is not proof of an
installed runtime version. Content-addressed blobs retain prior inputs, and
`--download-lock PATH` replays a previous downloads.json without refreshing,
rejecting missing lock entries or bytes that differ from its hashes. A lock can
replay an old mutable installer only while its bytes are cached or still served
by its source URL; it does not freeze an installer's subsequent network requests.

Actions restores previous caches but saves refreshed entries under a new run
key, since Actions cache entries are immutable. Legacy basename caches are not
trusted or migrated. The full nightly failure gate and app assertions are unchanged.
