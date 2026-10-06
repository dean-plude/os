## CI orchestration rebuilt

Replace duplicated build-and-test jobs with a reusable image build, verified
image artifacts, separate core/network/devices/graphics/compatibility jobs and
always-present CI result reporting. Rebuild nightly orchestration around
explicit PR scope, paginated fallback decisions, one app per VM/job, independent
SMP and complete per-app reports. Preserve release artifacts and publication
gates, scheduler race fixes, strict WebView2 assertions, download provenance,
mutable refresh and accurate skipped/not-run reporting.

Centralize runner dependencies, settle KVM permissions and probe VM creation.
Preserve full callback stress while joining teardown between batches. Correct
TCG CPUID recognition without changing timing thresholds. Compare decoded
NetSurf client pixels using reported geometry, eliminating desktop-clock noise
while retaining page assertions. Host regressions exercise scope, job outcomes,
workflow graph, image corruption, app selection/report completeness, fallback
pagination, KVM probing and graphics comparison. See `docs/ci.md` for required
check migration, reruns and runtime validation limits.
