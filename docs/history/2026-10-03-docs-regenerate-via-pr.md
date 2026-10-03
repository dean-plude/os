## Generated docs are rebuilt by pull request

Once main only took pull requests, the Docs workflow could neither push the
rebuilt README, ROADMAP, HISTORY and building.md regions nor open a pull
request, and the regions fell about 600 lines behind.  The docs check
(`tools/docgen.py --check-pr`) now accepts a changed generated region when
it is byte-identical to a fresh `tools/docgen.py` run on the pull request's
tree, and still rejects a hand edit.  The Docs workflow only reports stale
files.  The daily "Docs sync" pull request regenerates the regions and
lands through auto-merge.
