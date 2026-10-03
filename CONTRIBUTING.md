# Contributing to NovaOS

Many changes are in flight at once, so the repository is laid out to keep
them from colliding: **add a file instead of editing a shared list.**  Lists
that every change used to append to (the DLL table, the self-tests, the
README's tables, HISTORY, the roadmap) are now directories with one file
per item.  Two pull requests that each add an item touch different files
and merge cleanly in either order.

## Where each kind of change goes

| You are adding... | Add this file | Not this |
|---|---|---|
| a system DLL | `userland/NAME/dll.json` (+ `userland/NAME/build.py` if it needs a library or flags) | the old `DLLS` list in `tools/build_userland.py` |
| a program, or making one 32-bit or a System32 one | `userland/programs/NAME.json` (`x86`, `system`, `libs`, `msstl`, `selftest`) | `PROGRAMS_X86` / `SYSTEM_PROGRAMS` |
| export ordinals for a DLL | `"ordinals"` in its `dll.json` | `ORDINALS` |
| a CI self-test | `tests/selftest/core/NNN-name.py` (or `graphics/`) | `CORE` / `GRAPHICS` in `tools/selftest.py` |
| a nightly app-corpus program | `tests/appcorpus/NNN-name.py` | `APPS` in `tools/appcorpus.py` |
| a HISTORY section | `docs/history/YYYY-MM-DD-slug.md` (starts with `## Title`) | `docs/HISTORY.md` |
| a roadmap item, or ticking one off | `docs/roadmap/{graphics,apps,kernel,hardware}/NNN-slug.md` (edit the item's own file to tick it) | `docs/ROADMAP.md`'s "What comes next" |
| a row in README's program table | `docs/readme/programs/NNN-slug.md` | the table in `README.md` |
| a "What is inside" item, or extending one | `docs/readme/inside/NNN-slug.md` | the list in `README.md` |
| a bundled licence | `docs/readme/licenses/NNN-slug.md` (one "Name: licence" line) | the licence sentence in `README.md` |
| a row in building.md's self-test program table | `docs/selftests/NNN-name.md` | the table in `docs/building.md` |
| a program in the compatibility list, or a change to its status | `docs/compatibility/NNN-slug.md` | the table in `docs/compatibility.md` |

Files are taken in name order, so the prefix places the item: pick a number
between its neighbours (`045` between `040` and `050`).  Two changes that
pick the same number still merge (the names differ); the slug orders them.
HISTORY sections use the date instead, so they stay in the order they
landed.  `docs/building.md` and `tools/docgen.py` say what each file holds.

The fixed load addresses of the older DLLs stay in their `dll.json`; a new
DLL leaves `base` out and gets a free slot, so two branches can no longer
give their DLLs the same address (three open branches once all chose
0x7FFE50000000).

## Generated text: leave it alone in pull requests

`README.md`, `docs/ROADMAP.md`, `docs/HISTORY.md`, `docs/building.md` and
`docs/compatibility.md` have regions between `<!-- BEGIN generated:NAME -->` and
`<!-- END generated:NAME -->`.  `tools/docgen.py` builds them from the
fragment files above (and from the self-test and app-corpus files' `DOC`
strings, and the programs' `"selftest": true`).  In a pull request:

- add or edit fragments, and **do not edit or regenerate the regions**:
  CI's "Checks" job fails if a region differs from main's and is not
  byte-identical to a fresh `tools/docgen.py` run on the PR's tree
  (`tools/docgen.py --check-pr`);
- edit text outside the regions as usual (that is ordinary prose and
  conflicts only when two changes rewrite the same sentence);
- to preview the result, run `python3 tools/docgen.py`, look, and then
  `git checkout README.md docs/` before committing.

Main only takes pull requests, so nothing pushes the rebuilt regions to
it.  The daily "Docs sync YYYY-MM-DD" pull request (or any pull request
whose only generated-region change is an exact `python3 tools/docgen.py`
run) regenerates them, and the check accepts exact regenerations.  The
**Docs** workflow (`.github/workflows/docs.yml`) is a read-only report: it
runs `tools/docgen.py --check` on main and lists the files that are behind.

The standing rule still holds: every change updates the docs.  It now does
so by adding or editing fragments.

## Merging main into a branch

- Merge (do not rebase or force-push a branch someone else may have).
- Resolve every conflict keeping **both** sides' additions; a conflict in a
  generated region resolves by taking main's side (`git checkout --theirs`)
  and moving your text into a fragment.
- CI's **Checks** job runs `tools/ci/check-conflict-markers.py`, which fails
  on `<<<<<<<`, `=======`, `>>>>>>>` lines and on a line holding only a
  branch name (`main`, what is left of a deleted `>>>>>>> main`).  Run it
  before pushing a merge.
- `nova.iso` is not committed; if a merge brings it back, `git rm nova.iso`.

## Moving a branch from before this layout

A branch started before the change to one file per item conflicts once
when it merges main.  Resolve it like this:

1. `git merge origin/main`.
2. `tools/build_userland.py`: take main's (`git checkout --theirs
   tools/build_userland.py`).  For each DLL the branch added to `DLLS`,
   write `userland/NAME/dll.json` with its `deps` and no base; a changed
   dependency list goes in the existing DLL's `dll.json`; ordinals go in
   `"ordinals"`; a DLL-specific helper (a library build, flags) moves to
   `userland/NAME/build.py`.  Programs added to `PROGRAMS_X86` or
   `SYSTEM_PROGRAMS` get `userland/programs/NAME.json`.  Changes to the
   build itself (not to one DLL) stay in `tools/build_userland.py`.
3. `tools/selftest.py` and `tools/appcorpus.py`: take main's, and put each
   added test or program in its own file under `tests/selftest/` or
   `tests/appcorpus/`.
4. `README.md` and `docs/*.md`: take main's (`git checkout --theirs`),
   then put the branch's text in fragments: a new HISTORY section as
   `docs/history/YYYY-MM-DD-slug.md`, roadmap items in their
   `docs/roadmap/` file, table rows and list items in `docs/readme/` or
   `docs/selftests/`.  Changes outside the generated regions are re-applied
   to main's text directly.
5. Run the checks below; the generated regions must be exactly main's, or an exact regeneration.

## Before pushing

These check for conflict markers, that the DLL and program manifests and
the self-test files load, and that the generated doc regions match:

```bash
python3 tools/ci/check-conflict-markers.py
python3 tools/build_userland.py --check
python3 tools/selftest.py --list
python3 tools/docgen.py --check-pr origin/main
```

Shell blocks in the docs carry no `#` comments, trailing or on their own
line: zsh, the macOS default shell, does not treat `#` as a comment when
commands are pasted in, so explanations go in the text around the block.

and build and run the self-tests as [docs/building.md](docs/building.md)
describes.  System calls keep Windows 10 1903 x64 numbers.
