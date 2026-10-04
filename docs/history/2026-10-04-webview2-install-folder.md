## WebView2: the runtime installs (drive C:'s root permissions)

The WebView2 runtime's own setup (Chromium's `setup.exe`) copied the
runtime into `C:\AppData\Local\Microsoft\EdgeWebView\Application`, then
failed to create `SetupMetrics` there with "Access is denied" and rolled
the install back.  Before that, it gives the runtime's sandboxed
processes read access to the folder: it reads the folder's DACL, adds one
entry with `SetEntriesInAcl` and sets it back with `SetNamedSecurityInfo`.
Unless drive C: was on NTFS, nothing on it had a DACL (as on FAT, where
everyone may do anything), so the one added entry became the folder's
whole DACL and left the user out.  Nothing in the runtime is changed:
NovaOS now answers as Windows does.

- **Drive C:'s root permissions**: a root without a descriptor of its own
  (C: on FAT or in memory) has the DACL Windows gives `C:\`, the one a new
  NTFS volume's root already got: SYSTEM and Administrators full control,
  CREATOR OWNER (the user) full control of what is below, Authenticated
  Users change, Users read and execute, all inherited.  Every folder and
  file without a descriptor of its own inherits it, so an entry an
  installer adds joins those rather than replacing them.
- **Where the WebView2 install stops now**: it does not.  The setup
  finishes, Edge Update records the runtime as installed
  (`InstallApp returned 0x0`), and the corpus test runs it under TCG in
  about five minutes.  Next is running the runtime itself
  (`msedgewebview2.exe`, Chromium with its sandbox).
- `acltest` checks the root's DACL, its inheritance, and the setup's
  sequence on a new folder (122 checks, 64- and 32-bit).
