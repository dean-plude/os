## Main's boot-test no longer fails on a runner without KVM

The CI run on main after the merge of #200 (2026-10-04 09:07) went red in one
second: the hosted runner had no usable `/dev/kvm` and the step
`tools/ci/enable-kvm.sh` stops the boot-test there on pushes to main (the
rule added with the nightly corpus guard).  Nothing in the code was wrong, and
nobody can re-run a job, so a runner lottery left main red and skipped the
"latest" release.  Boot-test now falls back to TCG with a warning on every
event, as it already did on pull requests; the app corpus job keeps its strict
KVM requirement.  No timeouts or tests changed.
