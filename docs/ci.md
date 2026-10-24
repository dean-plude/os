# CI

NovaOS is tested by GitHub Actions. Five workflows matter:

| Workflow | File | Runs on | What it does |
|---|---|---|---|
| **CI** | `.github/workflows/ci.yml` | every pull request, merge-queue group, push to `main`, manual run; called by Release | builds one image, boots it in QEMU and runs every suite; on `main` republishes the rolling `latest` release |
| **Nightly app corpus** | `.github/workflows/nightly.yml` | three daily schedules, manual run, pull requests that change the corpus or what it depends on (a paths filter) | runs real Windows programs (`tools/appcorpus.py`) and an SMP stress test |
| **MS-DOS** | `.github/workflows/msdos.yml` | push to `main`, manual run, pull requests that change `tools/msdos/`, ntvdm or bootRogue (a paths filter) | builds MS-DOS 4.0 from source and boots it in QEMU and on ntvdm's CPU ([dos.md](dos.md)) |
| **Release** | `.github/workflows/release.yml` | a pushed `v*` tag | runs CI on the tag and publishes a GitHub release ([releasing.md](releasing.md)) |
| **Docs** | `.github/workflows/docs.yml` | push to `main`, manual run | read-only report of generated doc regions that are behind their fragments |

`.github/workflows/build.yml` is not run on its own. CI and the nightly
workflow call it to build the image.

[testing.md](testing.md) describes what the suites test. This page covers
how the jobs fit together, what each check means and how to deal with a
failure.

## The CI workflow

| Job (check name) | Needs | What it does |
|---|---|---|
| **Checks (conflict markers, manifests, generated docs)** | | decides the run's scope (`tools/ci/plan.py`), runs actionlint, the host regressions in `tests/tools/`, `tools/ci/check-conflict-markers.py`, `tools/ci/check-driver-imports.py`, `tools/build_userland.py --check`, `tools/selftest.py --list` for every suite, `tools/compat_smoke.py --list`, and `tools/docgen.py --check-pr` against the pull request's base |
| **Build** | Checks | `build.yml`: kernel, boot loader, userland, `build/nova.img` and `build/nova.iso`; stamps `build/ci-image.json` |
| **OS tests (core)**, **(network)**, **(devices)** | Build | `tools/selftest.py --suite SUITE`, one job per suite, `fail-fast: false`; the devices suite runs as three jobs, **OS tests (devices 1/3)** to **(devices 3/3)**, each a third of its boots (`--shard I/N`, balanced by test count) so no job nears the 60-minute limit; the install step (`.github/actions/deps`) has a 25-minute limit because a runner's apt can stall for a whole job, see [Runners, KVM and TCG](#runners-kvm-and-tcg). Two short checks ride along as steps instead of jobs of their own: **OS tests (core)** then runs the compatibility smoke (`tools/compat_smoke.py`: seven bounded core tests, `dlltest` x64 and x86, wait migration x64 and x86, Chromium start-up x64, `unwindtest`, `wvstarttest`, in their own boot, about a minute), and **OS tests (network)** then runs the performance budget (`tools/ci/perf.py`, [below](#the-performance-budget), about a minute). Both run even when the suite before them failed, but not when the image was never downloaded and verified (for example after an apt stall in the install step), so an infrastructure failure shows one error, not two |
| **Graphics tests (OpenGL, Direct3D)** | Build | builds or restores Venus, VKD3D-Proton and a QEMU with Venus, then `tools/selftest.py --suite graphics` |
| *(MS-DOS workflow)* **MS-DOS 4.0 (built from source, booted in QEMU and on ntvdm's CPU)** | | builds MS-DOS 4.0 from Microsoft's source with its own tools in DOSBox (`tools/msdos/build_msdos.py`), boots it from a floppy image in QEMU (`tools/msdos/boot_test.py`) and on `ntvdm.exe`'s real-mode CPU and BIOS built for Linux, typing `VER`, `DIR`, `MEM`, `CHKDSK`, `COPY` and a `DEBUG` session, then running a `.COM` and an `.EXE` on a drive C: of the runner's files as `CreateProcess` starts them and `DIR`, `CD`, `COPY`, `REN`, `DEL` there, writing a file in `EDLIN` and playing the DOS game bootRogue with the arrow keys (`tools/msdos/vdm_test.py`); its own workflow (`msdos.yml`), not part of the required checks ([dos.md](dos.md)) |
| **Build and boot-test** | Checks, Build, the OS test jobs | summary: passes only if all of those passed, compatibility smoke and performance budget included (`tools/ci/results.py boot`) |
| **Publish nova.iso** | Build and boot-test, Graphics tests | push to `main` only: republishes the `latest` release ([below](#the-rolling-latest-release)) |

### How many checks a pull request runs

A pull request that changes code runs eight jobs: Checks, Build, OS tests
(core), (network), (devices 1/3), (devices 2/3), (devices 3/3) and Graphics
tests, plus the **Build and boot-test** summary, and lists **Publish
nova.iso** as skipped. Until October 2026 it ran about 68 check runs: CI
had separate Compatibility smoke, Performance budget, MS-DOS and **CI
result** jobs, and the nightly app corpus started on every pull request
and fanned out to one **App corpus (NN)** job per program (53) whenever
the pull request touched `tools/ci/`, `.github/` or `tests/tools/`. Now
the corpus and MS-DOS workflows start on a pull request only when its
paths filter matches, so most pull requests do not list them at all; the
corpus still runs every day on `main` and can be started on any branch by
hand (Actions, **Nightly app corpus**, **Run workflow**, pick the branch).

### The checks to require

Three check names are stable and are the ones meant for a branch rule on
`main`:

- **Checks (conflict markers, manifests, generated docs)**
- **Build and boot-test**
- **Graphics tests (OpenGL, Direct3D)**

**Build and boot-test** covers the compatibility smoke and the performance
budget too, since they are steps of the OS test jobs. **Corpus result**
from the nightly workflow also always reports when that workflow runs. Branch rules live in the repository settings, not in
this repository. Pull requests are required for `main`: nothing is pushed
to it directly.

**Build and boot-test** (like **Corpus result**) runs with `if: always()`. They fail when a job they
depend on failed, was cancelled, is missing, or was skipped when the scope
said it should run. A skip counts as a pass only when the Checks job said
the run had no code to test.

### Scope: documentation-only pull requests

`tools/ci/plan.py` diffs the checked-out tree against the base commit
named in the event (the pull request's base SHA, or the merge group's
`base_sha`). If every changed path is under `docs/`, ends in `.md`, or is
`LICENSE`, the run has `code=false`: the Checks job still runs, the build
and test jobs are skipped, and **Build and boot-test** passes.
Pushes, manual runs, tag builds, an empty diff and any other path
build and test everything. CI has no workflow-level path filters, so a
required check never sits waiting for a job that will not start; only the
nightly corpus and MS-DOS workflows, which hold no required check, have
them.

### One image per run

The Build job uploads one image as the `nova-image` artifact:
`nova.img`, `nova.iso`, `kernel.elf`, `bootx64.efi` and `ci-image.json`,
which records the commit and the SHA-256 of the other four files
(`tools/ci/image_manifest.py stamp`). Every job that boots it first runs
`tools/ci/image_manifest.py verify --sha "$GITHUB_SHA"`, so a changed or
missing file, or an image from another commit, fails before any test
starts. Build also uploads `nova-iso` (the ISO) and `nova-boot` (kernel and
boot loader) for the publish jobs, and `build-diagnostics` (the kernel
build's error log) when something fails.

A release run (`release: true`, from `release.yml`) also runs
`tools/fetch_firmware.py` before building, so the image carries the default
firmware set and the audio DSP firmware. Pull request builds do not.

### Runners, KVM and TCG

All jobs run on `ubuntu-24.04`. `tools/ci/install.sh PROFILE` installs what
a job needs: `build` adds the compilers, `runtime` is QEMU, OVMF and the test
tools, `graphics` adds MinGW, Xvfb, Mesa's Vulkan and GL drivers and the
QEMU build dependencies, and `corpus` and `vkd3d` add what those builds need.

The CI workflow's jobs install through `.github/actions/deps`, which keeps
the `.deb` files a profile downloaded in an Actions cache keyed by profile,
runner image version and `install.sh`. A job that finds them installs with
`dpkg` and never touches the package mirror, because a runner's apt can
stall for a whole job (OS tests (devices 2/3) on PR #522: two 10-minute
attempts with no output). On a cache miss `install.sh` uses apt with
30-second connection timeouts, three retries and a 2-minute dpkg lock wait,
bounds each attempt at 10 minutes, tries twice (the second time from
`archive.ubuntu.com`, because the runner's Azure mirror can crawl at about
100 KB/s without ever failing, so apt's mirror list never moves on), and prints the stuck apt and
dpkg processes when an attempt stalls. The nightly workflow calls
`install.sh` directly, without the cache.

`tools/ci/enable-kvm.sh` makes `/dev/kvm` usable and then checks that a VM
can actually be created (`tools/ci/probe_kvm.py`), not just that the device
file exists. In `optional` mode (the CI workflow's test jobs) a failed
probe falls back to TCG with a warning. In `required` mode (the nightly app
and SMP jobs) it fails the job unless the run was started by hand with
`allow_tcg`. The accelerator in use is written to the job summary.

### Caches

- **Compiler cache** (`build.yml`): ccache (capped at 1 GB so one entry
  fits), restored by prefix. Only runs on `main` save it, and at most once
  a day (key `ccache-v4-OS-ARCH-HASH-YYYY-MM-DD`, skipped on an exact hit),
  so the repository's 10 GB cache is not churned out by per-run entries.
  Pull request runs only restore, falling back to main's newest entry, which
  they can read. The hash covers `tools/build_userland.py` and
  `kernel/CMakeLists.txt`. A cold build (no entry) is slower than the old
  45-minute job limit, so the image job allows 90 minutes.
- **Firefox installer and Mesa archive** downloads, keyed on
  `tools/bundle_firefox.py` and `tools/bundle_mesa.py`.
- **Graphics job**: the Mesa, DXVK and 7-Zip downloads, Venus
  (`tools/build_venus.py`), VKD3D-Proton (`tools/build_vkd3d_proton.py`)
  and QEMU 10.2 with virglrenderer and Venus
  (`tools/ci/build-qemu-venus.sh`), each keyed on the script that builds it.
- **MS-DOS workflow** and **Build**: the MS-DOS source checkout and its build
  (`~/.cache/novaos/msdos`), keyed on `tools/msdos/build_msdos.py` (the
  image ships MS-DOS's boot disk, so the build makes it too).

### Artifacts

Each test job uploads its output even when it fails, kept for 14 days:
`selftest-core`, `selftest-network`, `selftest-devices-1` to `selftest-devices-3`, `graphics-out`,
`compat-smoke-out` (from the core job), `perf-out` (from the network job) and, from the MS-DOS workflow, `msdos` (the built MS-DOS files, NMAKE's
`BUILD.LOG` and the boot test's serial log). They hold the serial logs, a screenshot
after each test, sound recordings and a JUnit file where the suite writes
one. Each job's summary page has a table of results. The graphics job also
uploads `venus` and `vkd3d-proton` for the publish job, and writes
virglrenderer logs, a host-side watchdog log (`tools/ci/host_watch.py`)
and host CPU profiles (`tools/ci/host_profile.py`) into `graphics-out`.

### Cancellation

Concurrency groups are per workflow, per event and per pull request or ref.
A new push to a pull request cancels that pull request's older runs (CI,
the corpus and MS-DOS). Runs on `main`,
merge groups, schedules, manual runs and releases are never cancelled by a
newer run, although GitHub may still drop a run that is only queued.

## The rolling `latest` release

When a push to `main` passes **Build and boot-test** and the graphics tests, the **Publish nova.iso** job
republishes the GitHub release tagged `latest`:

1. **Require current main before publication.** The job checks that
   `refs/heads/main` on GitHub is still the commit it tested
   (`git ls-remote origin refs/heads/main`). If `main` has moved on, the job
   fails and publishes nothing; the newer commit's own run will publish. It
   checks again just before moving the tag.
2. It builds an update channel from the tested kernel and boot loader
   with `tools/mkupdate.py`, stamped with a development version
   (`NOVA_VERSION+dev.YYYYMMDDHHMMSS`, the commit time in UTC) and signed when
   the `NOVAOS_UPDATE_SIGNING_KEY` secret is set ([updates.md](updates.md#the-rolling-builds-channel)).
3. It force-moves the `latest` tag to the commit and uploads `nova.iso`,
   `nova.iso.sha256`, `venus.7z`, `vkd3d-proton.7z`, `kernel.elf`,
   `bootx64.efi` and `novaos-update.txt`, replacing the old files. The App
   Store's Venus and VKD3D-Proton entries download from this release.
4. Since `v0.1.0` exists, `latest` is published as a pre-release and never
   takes GitHub's "Latest" mark, so installed systems on the default update
   channel are not offered it.

The job has its own concurrency group (`publish-iso`) and is never
cancelled. Tag builds skip it: `release.yml` publishes instead.

## The performance budget

Roadmap step 26.4. The performance budget (`tools/ci/perf.py`, a step of
the **OS tests (network)** job)
boots the tested image once, with four CPUs and 4 GB of memory, and
measures:

- **Boot time**: from QEMU starting to the kernel's main loop (the desktop
  is up).
- **Firefox start**: from Enter on `start firefox.exe` to its first page on
  the screen. The bundled Firefox opens a local page whose whole background
  is one colour (`C:\Perf\perf.html`), with a fresh profile whose `user.js`
  turns off the welcome, terms-of-use and default-browser pages. QEMU takes
  a screenshot twice a second until a fifth of the screen shows that
  colour.
- **smpstress scaling**: `smpstress scaling 0`. Every test must pass and
  nothing may hang. When bare system calls scale at least 2x (the virtual
  CPUs really ran side by side), files and the registry must scale at least
  half as well as system calls did in the same run; otherwise the figures
  are only recorded (`tools/ci/check-smpstress.py`, the same rule as the
  nightly SMP job).

The budget is in `tools/ci/perf-budget.json`: 20 s to boot and 10 s for
Firefox. The first KVM run took 9.8 s and 3.4 s, so only a large
regression fails. The step fails **OS tests (network)**, and with it **Build and boot-test**, when
a time is over budget, smpstress fails or scales too little, or Firefox
never draws its page. Times are judged only under KVM; under TCG they are
recorded but not judged.

The numbers, the budget and each verdict go to the job summary and to
`perf.json` in the `perf-out` artifact, with the serial log, the smpstress
log and a screenshot of Firefox. A change that makes something slower on
purpose raises the budget in the same pull request and says why.

## The nightly app corpus

`.github/workflows/nightly.yml` runs the programs in `tests/appcorpus/`
([testing.md](testing.md)).

| Job (check name) | What it does |
|---|---|
| **Gate (scope and nightly retries)** | runs the host regressions, decides whether to run (`tools/ci/nightly_gate.py`) and builds the app matrix (`tools/ci/plan.py --matrix`) |
| **Build** | `build.yml`, as in CI |
| **App corpus (NN)** | one job and one VM per program: `tools/appcorpus.py --only NAME`, at most four at once, `fail-fast: false` |
| **SMP stress** | `smpstress scaling 0` on four CPUs, judged by `tools/ci/check-smpstress.py` |
| **Corpus result** | validates and combines every program's report (`tools/ci/corpus_report.py`), requires every selected job (`tools/ci/results.py corpus`) and, on a scheduled run, posts the results to issue #210 and pushes the App Store's badges |

**When it runs.** Three schedules a day (03:17, 06:47 and 10:23 UTC). A
scheduled run goes ahead unless another scheduled or manual run is still
active or one in the last 20 hours ran the whole corpus successfully, so
the later schedules are retries. A manual run always goes ahead, on any
branch. On a pull request the workflow starts only when the pull request
changes the corpus or what it depends on: `tests/appcorpus/`,
`tests/reference/`, `userland/store/`, the corpus and download harness
(`tools/appcorpus.py`, `tools/download_cache.py`), the App Store code
(`kernel/apps/store.c`, `store_catalog.h`), the Firefox, Mesa and
VKD3D-Proton bundling scripts, or `nightly.yml` itself. The paths filter in
`nightly.yml` and `CORPUS_DIRS`/`CORPUS_FILES` in `tools/ci/plan.py` list
the same entries (a host regression checks that). Changes to `tools/ci/`,
`tests/tools/` or the other workflows no longer start it: the host
regressions in the Checks job test those scripts, and the next scheduled
run tests them against the programs.

**KVM.** The app and SMP jobs require KVM. A manual run can set `allow_tcg`
to accept software emulation.

**Reports.** Each app job uploads `appcorpus-NN` with its report, screenshots
and download provenance, and the SMP job uploads `smp-out`. A program that
fails, times out or produces no report fails **Corpus result**; skipped
programs (for example a sound test without PulseAudio) do not. A scheduled
run that ran the corpus posts the results (each program's table, the
smpstress log and a link to the run) as a comment on issue #210
(`tools/ci/corpus_comment.py`), and pushes each App Store program's
verdict to the `store-badges` branch for the App Store's badges
(`tools/ci/store_badges.py`, [app-store-catalog.md](app-store-catalog.md#nightly-badges)).
This is the only step that writes to the repository (`contents: write`);
it does not fail the run if the push fails.

**Downloads.** Each app job keeps its downloads in an Actions cache, but
every file is still checked against the URL, hash and refresh rules in
`tools/download_cache.py`. A program that needs VKD3D-Proton builds it (or
restores it from the cache) in its own job.

## Troubleshooting

**Checks failed.** Read the failing step. The usual causes:

- *Validate generated documentation*: a generated region differs from the
  base and is not an exact `tools/docgen.py` regeneration. See
  [CONTRIBUTING.md](../CONTRIBUTING.md#generated-text-in-pull-requests).
- *Validate manifests and suites*: conflict markers left by a merge, a
  broken `dll.json` or program `.json`, a self-test file that does not load,
  or a driver import NovaOS does not export.
- *Host regressions*: a unit test in `tests/tools/` failed.

**Verify tested image failed.** The job downloaded an image whose commit
or checksums do not match. Re-run the whole workflow, not just that job.

**KVM unavailable.** The job summary says "Infrastructure failure: KVM
unavailable". This is a runner problem, not a NovaOS one: re-run the job.
For the nightly workflow, a manual run with `allow_tcg` works around it
(slowly).

**A test failed.** Download the job's artifact. The serial log has every
kernel message; the screenshot taken after the failing test shows the
screen. Reproduce locally with `python3 tools/selftest.py --suite SUITE
--only NAME` after a build ([building.md](building.md#tests)).

**Publish nova.iso failed at "Require current main before publication".**
`main` moved on while the run was going. Nothing to do: the newer commit's
run publishes.

**Running the Checks job's steps locally.** These need no build and no VM:

```bash
python3 -m unittest discover -s tests/tools
python3 tools/ci/check-conflict-markers.py
python3 tools/ci/check-driver-imports.py
python3 tools/build_userland.py --check
python3 tools/selftest.py --suite core --list
python3 tools/compat_smoke.py --list
python3 tools/docgen.py --check-pr origin/main
```

The full build and boot tests run in the pull request's Actions run. A
green CI run shows NovaOS works in QEMU; it says nothing about physical
hardware.

## What the network and graphics suites run

The core and devices suites are described in [testing.md](testing.md).

- **Network** (the **OS tests (network)** job): three boots.
  - virtio-net on QEMU's user network: `ipconfig`, `ping`, Winsock over IPv4
    (`netcat`), `httptest suite` (WinHTTP with HTTP/2 by ALPN) and
    `schanneltest` against `tools/h2server.js`, `looptest` (Winsock over
    127.0.0.1 and ::1), `overlaptest`, `afdtest`, `loadtest`,
    `tcptabletest`, `adaptertest` (several 64- and 32-bit), `prioritytest
    net`, `dltest` (eight downloads at once), `netsh` switching to a
    static address and back to DHCP, and `winget` with the WinGet REST
    source in `tools/h2server.js` (source add, search, install of an older
    version, list, upgrade, uninstall, a download whose SHA-256 is wrong
    refused) and the App Store's catalog as a source; last, an App Store
    download as from a real site (`store install` with the test server's
    catalog: a redirect to a ~1,000-character signed address as GitHub's
    release downloads, a 503 the Store tries again after, and a body
    trickled over ~40 s).
  - virtio-net on an IPv6-only network served by `tools/v6peer.py`: SLAAC
    and RDNSS, `ping -6`, `curl -6` and Winsock over IPv6; stateful DHCPv6
    (the lease in `ipconfig /all` and `dhcp6test`, 64- and 32-bit, a
    Rebind at T2 and a Renew while `ping -6` runs, `ipconfig /release6`
    and `/renew6`).
  - an Intel e1000e (82574L): its PHY and link, the link pulled and plugged
    back, sleep and wake, then the IPv4 tests above again on it (not
    `winget` or the App Store download).
- **Graphics** (the **Graphics tests (OpenGL, Direct3D)** job): two
  monitors, the first a QEMU 3D virtio-gpu with Venus
  (`virtio-vga-gl,venus=on`, whose Vulkan and OpenGL are the runner's
  lavapipe and llvmpipe), run under Xvfb. In order:
  - `d2dtest` (Direct2D, and a scene that must match the reference
    `tools/d2dtest/reference.py` draws with Skia), `dwtest`, `dw3test` and
    `dw6test` (DirectWrite), `d3d9test none` (Direct3D 9 with no DXVK);
  - the App Store installs Mesa 3D, DXVK, VKD3D-Proton and Venus
    (`tools/ci/stage-graphics.sh` stages the downloads);
  - `montest` (the second monitor), `nstest` (NetSurf on a page with an SVG
    and a script);
  - `gltest` (15 tests, on virgl and on llvmpipe), `gltest colors`, `gltest
    fps`, `gltest present` (virgl frames shown straight from the host GPU,
    and through GDI when a window covers them) and `gltest monitors`;
  - `d3dtest` (17 tests, Direct3D 9/11 through DXVK on Venus), `d3d9test
    dxvk`, `d3dtest fps`, `d3dtest present`, `d3dtest angle`, `d3dtest
    shared`, `dcomptest dxvk`, `dcomptest life` and `dcomptest direct`
    (DirectComposition swap chains on Venus);
  - `d3d12test` and `d3d12test compute` (Direct3D 12 through VKD3D-Proton),
    `hlsltest` (NovaOS's HLSL compiler);
  - the App Store's and File Explorer's scroll bars, driven with the mouse.

  Most tests run both 64- and 32-bit and take a screenshot while they draw.
  The `fps` tests require the GPU path to be faster than the software one,
  and direct presentation to be faster than the GDI path.
