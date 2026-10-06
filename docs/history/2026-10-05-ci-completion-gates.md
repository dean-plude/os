## CI completion checks and cancellation scope

Every standard CI run executes host compatibility and workflow regressions
before building. The `CI result` check requires successful preflight checks
and successful build/boot and graphics jobs for code changes. Docs-only
build skips are accepted only with an explicit `code=false` output.
`Corpus result` requires the corpus job to succeed when the nightly gate
requests execution. Missing outputs, failed/cancelled dependencies and
unexpected skips fail the result checks, with a job-result summary. These
job results do not replace per-program passed/failed/skipped/not-run reports.
External cancellation of an entire workflow can prevent its summary job
from starting; GitHub still records the cancelled run.

Concurrency groups separate workflow events, and only superseded PR runs
cancel active work. Scheduled, manual, merge-group and release work retain
active execution. GitHub can still replace pending runs in a concurrency
group; the nightly retry gate continues to ignore cancelled runs. Existing
full suites, strict WebView2 assertions and the corpus/SMP failure gate remain.
The result checks are available for branch protection; this change does not
edit repository rules or remove existing required checks.
