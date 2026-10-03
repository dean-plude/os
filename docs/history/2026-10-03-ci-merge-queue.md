## Run CI on merge queue groups

GitHub's merge queue builds each queued pull request on a temporary
merge-group branch and only starts workflows that listen for the
`merge_group` event.  `.github/workflows/ci.yml` now does, so the three
required checks (Checks, Build and boot-test, Graphics tests) run for
queued pull requests.  The job names are unchanged.  The generated-docs
check stays pull-request only, since the pull request already passed it;
the other jobs have no conditions that depend on a pull request, and the
concurrency group falls back to the merge-group ref.
