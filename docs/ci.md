# CI execution and troubleshooting

CI builds one image per workflow run and distributes that image to independent
jobs. Every consumer checks its commit identity and SHA-256 manifest before
booting. A changed or missing artifact fails before tests start. The release
workflow continues to call CI and consumes `nova-iso` and `nova-boot`; Venus
remains a graphics artifact. Rolling main releases publish only after CI result
passes and the publishing commit is still the current main head.

| Check | Execution |
| --- | --- |
| Checks | Actionlint, host regressions, conflict markers, manifests, all suite inventories and generated-document validation |
| Build | Kernel, bootloader, userland, disk image and ISO; release builds include DSP firmware |
| Compatibility smoke | Seven existing bounded x64/x86 loader, synchronization, Chromium, unwind and WebView2 startup assertions |
| OS tests | Separate core, network and devices jobs, with sibling jobs continuing after a failure |
| Graphics tests | Existing full Mesa, DXVK, Venus and browser regression suite |
| CI result | Requires every selected job to succeed; rejects failed, cancelled, missing and unexpected skipped jobs |
| Corpus result | Same completion rule for the nightly build, all app matrix jobs and SMP; validates and combines every selected program's report |

Use **CI result** and **Corpus result** as the required checks when configuring
repository rules. They always exist, including for documentation-only PRs.
This change does not edit repository protection rules. The existing
**Build and boot-test** check is retained as an aggregate of the build and all
three OS suites; **Graphics tests (OpenGL, Direct3D)** keeps its existing name.
Existing protections can therefore continue to require those checks while
CI result covers compatibility and complete workflow execution as well.

`tools/ci/plan.py` diffs the checked-out PR or merge-group tree against the
base SHA declared in the event. Documentation-only changes skip native jobs
explicitly; pushes, manual invocations and unknown code inputs build and test.
Corpus PR execution is selected for corpus fixtures, reference images, host
regressions, CI configuration, download/harness code, the Store catalog and
Firefox bundling. Other kernel/API PRs retain full OS tests and compatibility
smoke. Scheduled and manual corpus runs always select the full corpus.
There are no workflow-level path filters that can leave a required check pending.

The nightly workflow retains three fallback schedules and the 20-hour success
window. Failed or cancelled runs and gate-only success permit another attempt;
active work and successful executed corpus runs suppress duplicates. Runs and
latest-attempt jobs are paginated. API errors fail the gate. PR runs can cancel
superseded PR runs; main, scheduled, manual and release work are not actively
cancelled by new commits. GitHub may replace older queued runs in a concurrency
group; completion checks reject cancelled required execution.

Each selected app runs in its own VM and job, with at most four app jobs running
at once. A failed or timed-out app cannot prevent sibling app jobs or the separate
SMP job from running. Isolation also gives each game's own controller fixture a
fresh VM. All programs in `tests/appcorpus/` are selected exactly once. Unknown,
empty or duplicate `--only` selections fail before download or boot. Application
assertions, screenshot tolerances, strict WebView2 milestones, mutable download
refresh and pinned replay behavior are retained. Reports preserve passed, failed,
skipped and not-run outcomes; an absent or incorrectly attributed report fails
aggregation. Genuine application compatibility failures remain failures.

Runner dependencies live in `tools/ci/install.sh`: build jobs install compilers;
image consumers install runtime tools; graphics adds its driver toolchain. KVM
selection waits for udev and verifies that the runner can create a VM through
KVM, instead of checking only file permissions. Bounded PR OS jobs allow TCG;
corpus and SMP jobs require KVM unless a manual `allow_tcg` invocation requests
software emulation. Accelerator selection is recorded in job summaries. KVM
infrastructure failure remains visible and can be retried by rerunning that job.

Compiler and host-tool caches include platform and builder identity. Application
download caches restore prior inputs but still use the existing URL/hash/refresh
validation. Every boot starts from its own downloaded image, so suites cannot
persist changes into another suite's image. Live raw serial logs are written directly into artifact directories and survive
VM cleanup or workflow interruption. Test jobs keep serial logs,
screenshots, download provenance and available JUnit results even on failure.
The image bundle includes `ci-image.json` with commit and file checksums.

The NetSurf layout comparison decodes PNG pixels and compares the actual client
geometry reported by the browser, excluding only its 18-pixel load-time status
bar. Desktop clocks cannot change a page verdict. Missing, changed or invalid
client geometry fails the comparison; all page pixels remain compared. The
network scheduling test now correctly recognizes QEMU's little-endian TCG CPUID
signature, retaining its existing 250 ms KVM and 500 ms TCG limits. Thread-pool
callback stress still submits all 5,120 callbacks and checks the exact handle
baseline, joining callback teardown between batches.

For local verification, run the host regressions, actionlint, manifest and
suite-list checks, documentation validation and conflict-marker check. Full
native build/boot validation is supplied by the PR's Actions runs. Rebuilding
or rerunning CI does not establish support for physical hardware or repair
unrelated app/runtime failures.

Corpus visual fixtures use the existing desktop menu to select Sunset, matching
our full-screen reference images. A checked desktop patch fails setup if theme
selection did not take effect. Normal OS boots keep Aurora, and screenshot
thresholds and reference images are unchanged. The selected app declares its
own networking independently of echo/HTTPS fixtures. VLC stages its own
30-second H.264/AAC color-bar clip instead of relying on another app's output.
WebView2 installation accepts the updater's bracketed success log format and
still requires a zero result and every WebView2 host milestone.
