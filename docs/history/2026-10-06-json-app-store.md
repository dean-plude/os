## Independently updateable App Store catalog

The App Store's 37 existing entries now live in `userland/store/catalog.json`
instead of a compiled C table. The OS image embeds that JSON as an offline
fallback at `C:\Windows\AppStore\default-catalog.json`; a refreshed copy at
`C:\Windows\AppStore\catalog.json` takes precedence when opening the Store.

**Refresh**, **F5**, or `store refresh` downloads the catalog over HTTPS and
replaces the saved copy after validating the complete document. The default
source is the repository's `main/userland/store/catalog.json`; an optional
`C:\Windows\AppStore\catalog-url.txt` selects another HTTPS endpoint.
Adding or removing entries, changing installer URLs, and updating compatibility
notes no longer require installing an OS update or restarting NovaOS.

The Store retains its current catalog after network errors, unsupported schemas,
malformed documents, unsafe installation paths, or failed cache writes. Cached
files are replaced through a temporary file and rename. Refresh cannot replace
app indices during an active download or unpack, and app actions wait during a
refresh. Closing the window releases an outstanding catalog request. Offline
operation uses the saved list or the bundled fallback.

The bounded native parser limits catalogs to 512 KiB and 256 apps, checks required
fields and UTF-8/JSON escapes, rejects duplicate app names and download filenames,
and validates archive destinations and system-file arguments. It preserves the
existing download, installation, runtime and OS-update behaviors. New catalog
entries describe packages; they do not add compatibility APIs to NovaOS.

Host regression tests execute the production parser and refresh state machine,
covering catalog edits, Unicode, limits, malformed JSON, path validation, failed
requests, write/rename failures, busy operations, successful replacement, and
saved/offline fallback. CI's Checks job runs these tests. Freestanding compilation
checks cover Store and Terminal; full boot and HTTPS refresh testing require CI.
See `docs/app-store-catalog.md` for the format and update procedure.
