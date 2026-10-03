## Firefox in the App Store

The last program of the Phase 20 catalog item before Krita: stock
Firefox, not the Floorp fork that Phase 16 was debugged with, is now a
catalog program that installs and runs unmodified.  Floorp 12.19 and
Firefox 157 share the engine, so the browser itself ran as Floorp did;
what changed in NovaOS is around it.

- **The catalog entry** downloads Mozilla's full installer for 157.0
  (`archive.mozilla.org`, pinned like the other entries instead of
  `download.mozilla.org`'s floating "latest").  The full installer is a
  7-Zip self-extractor holding `core\` (the browser) and `setup.exe`; the
  Store's Install button unpacks it into `C:\Programs\Mozilla Firefox`
  and Open starts `core\firefox.exe`.
- **The nightly corpus installs it through the Store.**  An app-corpus
  program can now name its catalog entry (`App(store=...)`): its download
  must be the catalog's, it is put in `C:\Downloads` under the catalog's
  file name with 7-Zip in `C:\Programs\7-Zip`, and
  `Test(store=...)` runs `store install NAME` and waits for the Store's
  "Installed" line, so the corpus tests the same path a user's click
  takes.  Unpacking the installer takes a few minutes under TCG.
- **An HTTPS page.**  `tools/appcorpus.py` runs an HTTPS server on the
  host for programs that ask for it (`App(https=True)`), with a
  certificate for 10.0.2.2 from a CA made for the run.  Firefox trusts
  that CA through `distribution\policies.json` beside `firefox.exe`,
  Mozilla's documented way to configure a deployment, which also turns
  off the first-run pages, update checks, telemetry and the terms-of-use
  prompt (a dimmed tab-modal sheet on a new profile).  The page's
  screenshot is `tests/reference/firefox.png`.
- **Firefox's launcher process.**  `firefox.exe` starts a second
  `firefox.exe` (the browser) suspended, installs its DLL blocklist hooks
  in it, resumes it and exits.  The hooks' trampolines live in a
  `SEC_RESERVE` section the launcher commits and writes through its own
  view and maps into the child near ntdll (`MapViewOfFile3` with an
  address range, or `NtMapViewOfSection` with the child's process handle).
  NovaOS mapped views only into the calling process, so the launcher
  logged its failure (HRESULT 0x80070507 from `DllBlocklistInit.cpp`),
  ended the child, turned the launcher off in the registry and ran the
  browser itself.  `NtMapViewOfSection`, `NtMapViewOfSectionEx` and
  `NtUnmapViewOfSection` now take another process's handle, and
  committing pages of a section view succeeds (a section's pages are
  always there); `shmtest` checks both.  `ReportEventW` also prints an
  event's binary data, which is how the source file showed up.
- **Window titles.**  The desktop's title bars and the taskbar draw ASCII,
  and every other character of a window's title became `?`, so Firefox's
  "Page — Mozilla Firefox" read "Page ? Mozilla Firefox".  Dashes,
  curly quotes, non-breaking spaces, bullets and the ellipsis now get
  their nearest ASCII form, as Windows' best-fit code pages give them.

Firefox's other `[UM]` lines are harmless probes: `mfplat.dll` (Media
Foundation, it falls back to its own decoders), `profapi.dll` for the
Windows App SDK, `D3DKMTQueryStatistics`, and WinRT activation looking
for `Windows.UI.dll` and friends.  Still open: a publicly trusted HTTPS
site (the test network has no internet), and Thunderbird, which shares
the runtime but is untested.
