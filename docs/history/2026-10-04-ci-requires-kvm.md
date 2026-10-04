## CI says which accelerator it used and stops without KVM

The nightly app corpus run of 2026-10-03 23:44 (pull request on a branch of
#166) landed on a runner without `/dev/kvm`.  `tools/ci/enable-kvm.sh` printed
a warning and fell back to TCG, the job went on for 28 minutes and its table
could not confirm the KVM-only fixes.  Now the app corpus job, and boot-test on pushes to main, fail in that step with a clear message when KVM is missing, and the job
summary and the corpus table header state "KVM" or "TCG".  A workflow started
by hand has an `allow_tcg` input for a deliberate TCG run.  The graphics job
keeps its TCG fallback, and so does boot-test on a pull request (a required check must not go red on the runner alone; the first try failed a PR that way).  No timeouts or tests changed.
