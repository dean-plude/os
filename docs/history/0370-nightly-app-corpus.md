## Nightly app corpus

- **`tools/appcorpus.py`** downloads the official Windows x64 releases of
  ripgrep, fd, jq, 7-Zip, MinGit, Python (the NuGet package), Node.js and
  Notepad++ (portable), unpacks them into `C:\Apps` with a few sample files
  and a bare git repository, boots once and types each program's commands:
  a search, a `find`, a JSON filter, an archive made and tested, `git
  clone`, `log` and `status`, `python -c`, `node -e`.  Notepad++ opens a
  file and its screenshot is compared with `tests/reference/notepad++.png`
  (scaled down; at most 3% of pixels may differ).
- **`.github/workflows/nightly.yml`** runs it every night on main (and on
  pull requests that change the corpus) and posts the pass/fail table to
  the run's summary and as a comment on the "Nightly app corpus" issue.
- Not yet: Notepad++'s tab bar and status bar still draw black; the
  reference shows them so, and an improvement means updating it
  (`--update-reference`).
