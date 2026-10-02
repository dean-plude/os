## One file per item: parallel changes without merge conflicts

Up to seven pull requests were open at once, and nearly every one edited
the same lines of the same files: the DLL table and program sets in
`tools/build_userland.py`, the test lists in `tools/selftest.py` and
`tools/appcorpus.py`, and the lists in the README, `docs/HISTORY.md`,
`docs/ROADMAP.md` and `docs/building.md`.  Each merge left the others
conflicted, and twice conflict markers reached main.  Those lists are now
directories with one file per item, so changes add files instead of
editing shared lines.  [CONTRIBUTING.md](../CONTRIBUTING.md) is the guide.

- **DLLs**: `userland/NAME/dll.json` registers each DLL (dependencies,
  load addresses, extra source directories, entry point, implicit TLS,
  export ordinals); `tools/build_userland.py` finds them and links each
  after its dependencies.  A DLL that needs more (Mbed TLS for secur32,
  the msvcrt/ucrtbase double link) keeps that code in its own
  `userland/NAME/build.py`.  The existing DLLs keep their addresses; a
  new DLL leaves them out and gets a free 16 MiB slot, so two branches can
  no longer pick the same address (three open ones had all chosen
  0x7FFE50000000), and the build stops if two DLLs' images overlap.  The
  userland it builds is byte-for-byte the same as before (link timestamps
  aside).
- **Programs**: `userland/programs/NAME.json` replaces the 32-bit and
  System32 sets (`x86`, `system`, extra `libs`, `selftest`).
- **Tests**: one file per self-test in `tests/selftest/core/` and
  `graphics/`, one per program in `tests/appcorpus/`, run in file-name
  order; `tools/selftest.py --list` prints a suite.
- **Docs**: README's program table, "What is inside" list, licences,
  core-suite and self-test lists, the roadmap's "What comes next" items,
  building.md's self-test table and every HISTORY section are built by
  `tools/docgen.py` from files in `docs/readme/`, `docs/roadmap/`,
  `docs/selftests/` and `docs/history/` (and the tests' `DOC` strings).
  Pull requests add fragments and leave the generated regions alone; the
  Docs workflow (`.github/workflows/docs.yml`) rebuilds them on main after
  each merge.
- **CI**: a quick Checks job runs before the boot tests: no conflict
  markers or stray branch-name lines (`tools/ci/check-conflict-markers.py`),
  the manifests and test files load, and a pull request has not edited a
  generated region by hand.
