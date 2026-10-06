## A bounded compatibility smoke suite before the full boot tests

PRs changing kernel, userland, headers, build definitions, core test definitions
or the smoke harness run seven existing regressions on four CPUs: dlltest on
x64 and x86, waitmigrationtest on x64 and x86, chrometest on x64, unwindtest and wvstarttest.
They exercise loader/TLS, scheduler migration and waits, shared-memory mapping/access rights, exception
unwinding and browser runtime APIs. Each retains its core assertions and exit
checks; starting successfully alone cannot pass. The per-test limits total at
most 21 minutes and the workflow step has a 25-minute limit. There are no external
installers, audio tests, deliberate panics or reboots in this selection.

The suite runs immediately after the existing build, before the longer boot
suites. Serial logs, screenshots and JUnit are kept in compat-smoke-out. Missing
or duplicate test definitions fail host validation, and missing VM results fail
the suite. CI runs host regressions on every PR.

Configure `Compatibility smoke` as a required check alongside `Checks (conflict
markers, manifests, generated docs)`, `Build and boot-test` and `Graphics tests
(OpenGL, Direct3D)`. The smoke check always runs: out-of-scope PRs explicitly
report the scope skip as success; an in-scope smoke failure, cancellation or
missing execution fails. There is no workflow-level path filter that leaves a
required check pending. Repository branch rules are not changed by this PR.
The existing full core suites and nightly corpus/failure gate remain intact.
