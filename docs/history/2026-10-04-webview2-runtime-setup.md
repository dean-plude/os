## WebView2: the runtime's setup unpacks its archive; wer.dll

The WebView2 runtime's own setup (Chromium's `setup.exe`, which Microsoft
Edge Update starts) stopped with "Can't map file to memory: Incorrect
function" while unpacking its archive, then crashed on the missing
`wer.dll`.  The archive, `MSEDGE.7z`, is 728 MB, and NovaOS refused any
section (file mapping) over 256 MB with `STATUS_SECTION_TOO_BIG`, which
ntdll had no Win32 error for, so it came back as `ERROR_INVALID_FUNCTION`.
Nothing in the runtime is changed: NovaOS now maps the archive as Windows
does.

- **Large file mappings**: a section may now be as large as free memory
  allows (less a sixteenth of the machine's, kept for the kernel); when
  memory is short, the saved files of drive C: that nothing holds are let
  go of first.  A file mapping is filled from the file, and written back
  to it, a megabyte at a time, so a large one no longer holds the desktop
  up.  A read-only mapping larger than its file fails with
  `STATUS_SECTION_TOO_BIG` (only a writable one extends the file), and
  ntdll turns that, `STATUS_MAPPED_FILE_SIZE_ZERO` and
  `STATUS_COMMITMENT_LIMIT` into Windows' errors instead of
  `ERROR_INVALID_FUNCTION`.
- **`wer.dll`**: Windows Error Reporting's report API (`WerReportCreate`,
  `WerReportSetParameter`, `WerReportAddFile`, `WerReportAddDump`,
  `WerReportSetUIOption`, `WerReportSubmit`, `WerReportCloseHandle`,
  `WerAddExcludedApplication`).  NovaOS has no reporting service, so it
  answers as a Windows machine with reporting turned off: reports are made
  and closed, and submitting one gives `WerDisabled`.  Written from the
  documented API; no reporting code exists under a licence NovaOS reuses.
- **Device family**: ntdll's `RtlGetDeviceFamilyInfoEnum` (a desktop
  running Windows 10.0.19045; `setup.exe` stops unless it is told
  "desktop"), kernel32's `OOBEComplete` and `FlsGetValue2`.
- **Tests**: the new `wvsetuptest` maps a 300 MB file whole through a
  duplicate of its handle, as `setup.exe` does, and checks `wer.dll`'s
  report API, the device family, `OOBEComplete` and `FlsGetValue2`.
- **Where the install stops now**: `setup.exe` unpacks all of
  `MSEDGE.7z` and copies the runtime into
  `C:\AppData\Local\Microsoft\EdgeWebView\Application`, then cannot make
  the `SetupMetrics` folder there ("Access is denied") and rolls the
  install back (exit code 521)
  ([compatibility.md](../compatibility.md#webview2)).
