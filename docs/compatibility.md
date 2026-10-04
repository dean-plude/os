# Program compatibility
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

Which Windows programs run on NovaOS, how well, and how that was
checked.  Every program here is the official, unmodified release; when one
fails, NovaOS is what gets fixed.  All of it was checked in QEMU: nothing
has been run on a real PC yet ([hardware.md](hardware.md)).

What the columns mean:

- **Status**: *Works* (does what the "What works" column says, which
  covers what most people use it for), *Partly* (starts and does some of
  it, with a known problem or a large part unchecked), *Untested* (in the
  App Store, but nobody has checked it yet; expect it to fail).
- **Checked by**: *nightly corpus* is a script in `tests/appcorpus/` that
  downloads the program, runs it in QEMU and checks its output or a
  screenshot (`tools/appcorpus.py`, run by the "Nightly app corpus" workflow);
  *CI graphics tests* run on every pull request; *core self-tests* run on
  every pull request too; *by hand* means someone ran it in QEMU and it
  is not re-checked automatically, so it can break unnoticed.

Each row is a file in `docs/compatibility/`; change a program's row there
when its status changes (CONTRIBUTING.md).  Roadmap step 20.7 will have
CI keep the statuses and the App Store's notes in step with the nightly
results; until then they are kept by hand.

<!-- BEGIN generated:compat-table -->

| Program | Status | What works | Checked by |
|---|---|---|---|
| **7-Zip 26.03** (x64) | Works | Installs from its own setup; the file manager, the add and extract dialogs, dragging files out of archives; `7z.exe` packs and tests archives | nightly corpus |
| **MinGit 2.47** (Git for Windows) | Works | `clone`, `commit`, `log`, `diff`, `merge`, `fetch` and `push` between local repositories, paged by `less` | nightly corpus |
| **ripgrep 14.1, fd 10.2, jq 1.7** | Works | Searching, walking folders and filtering JSON from the Terminal | nightly corpus |
| **bat, fzf** | Works | Printing files with highlighting, fuzzy finding | by hand in QEMU |
| **Python 3.14** (NuGet package) and **NumPy 2.5.3** | Works | Scripts with hashlib, JSON, regular expressions, threads and subprocesses; NumPy gives the same results as on Linux | nightly corpus (Python), by hand (NumPy) |
| **Node.js 24** (`.msi` and `.zip`) | Works | `node`, `npm`, crypto, files and timers; the `.msi` installs and uninstalls | nightly corpus |
| **.NET 10** | Works | Console programs and the C# compiler; German and Japanese dates and numbers format as on Windows | nightly corpus |
| **Eclipse Temurin 21** (`.msi` JRE, `.zip` JDK) | Works | `java`, `javac`, threads, exceptions and files | by hand in QEMU |
| **ffmpeg 7.1.1** | Works | Converts an H.264 MP4 to WebM | nightly corpus |
| **Neovim 0.10 and 0.11**, **MSYS2 `sh`** (from MinGit) | Works | Neovim opens, edits and saves a file; interactive `sh` sessions with pipes, subshells and globbing | by hand in QEMU |
| **Windows Installer (`.msi`) and NSIS setup programs** | Works | 7-Zip, CMake, Node.js, Temurin and KeePassXC packages install, make shortcuts and uninstall, custom actions included; NSIS installers and their uninstallers run | core self-tests (`msitest`, `msiscript`), by hand |
| **Notepad++ 8.8.3** (x64 portable) | Works | Opens, edits and saves files through the Open and Save As dialogs | nightly corpus |
| **SumatraPDF 3.4.6** (x86 portable) | Works | Opens a PDF and renders its pages; printing finds no printer | nightly corpus |
| **WinMerge 2.16.50** | Works | Compares two files side by side | nightly corpus |
| **PuTTY 0.81** | Partly | Raw connections work; SSH is untested | nightly corpus |
| **KeePassXC 2.7.12** (Qt 5) | Works | Opens and unlocks a KDBX 4 database and shows its entries; Windows Hello unlock reports "not supported" | nightly corpus |
| **VLC 3.0.21** (Qt 5) | Partly | Plays an H.264 and AAC MP4 with sound in its window; closing VLC can freeze NovaOS (being fixed) | nightly corpus |
| **Audacity 3.7.4** (wxWidgets) | Partly | Records from the microphone, draws the waveform and saves an `.aup3` project; recordings can have short dropouts (being fixed) | nightly corpus |
| **Inkscape 0.91** (GTK 2) | Works | Opens, edits and saves an SVG; Inkscape 1.x is untested | nightly corpus |
| **Krita 5.3.4** (Qt 5) | Partly | Starts and opens a new image on Mesa 3D (install Mesa 3D from the App Store first); painting and saving are not checked yet | nightly corpus |
| **Firefox 157** and **Floorp 12.19** | Works | Installs from the App Store, loads pages over HTTP and HTTPS, scrolls and takes typing in forms; a publicly trusted HTTPS site is untested (the test network is offline) | nightly corpus |
| **Mesa 3D 24.2.4** (App Store, Runtimes) | Works | OpenGL 4.5 on the CPU (llvmpipe) and Vulkan (lavapipe), 64- and 32-bit | CI graphics tests (`gltest`) |
| **DXVK 2.5.3** (App Store, Runtimes) | Works | Direct3D 8 to 11 on Vulkan, 64- and 32-bit | CI graphics tests (`d3dtest`) |
| **Venus** (App Store, Runtimes) | Works in QEMU | Vulkan, and Direct3D through DXVK, on the host GPU when QEMU gives NovaOS a 3D virtio-gpu | CI graphics tests |
| **.NET Desktop Runtime 8** (App Store) | Partly | Console programs run; WinForms and WPF programs are untested | by hand |
| **Thunderbird** (App Store) | Untested | Uses the same engine as Firefox | — |
| **GIMP** (App Store) | Untested | | — |
| **HandBrake** (App Store) | Untested | Needs the .NET Desktop Runtime | — |
| **OBS Studio** (App Store) | Untested | | — |
| **LibreOffice** (App Store) | Untested | | — |
| **qBittorrent** (App Store) | Untested | | — |
| **WinSCP** (App Store) | Untested | | — |
| **ShareX** (App Store) | Untested | Needs the .NET Desktop Runtime | — |
| **Visual C++ Redistributable** (App Store) | Untested | NovaOS has its own `vcruntime140` and `msvcp140`, so most programs do not need it | — |
| **Microsoft Build of OpenJDK 21** (App Store) | Untested | Temurin 21 (above) works | — |

<!-- END generated:compat-table -->

## Roblox and anti-cheat

Roblox's installer works on NovaOS (it needed the firmware tables
`GetSystemFirmwareTable` reads and Windows' answer for a registry key
below a missing one).  The client, `RobloxPlayerBeta.exe`, starts with
Hyperion, Roblox's anti-cheat (`RobloxPlayerBeta.dll`), which decrypts the
game at run time and makes its own `syscall` instructions instead of
calling ntdll.  It does not read ntdll's stubs for the service numbers;
it works them out the way the kernel assigns them, by ranking ntdll's `Zw`
exports by address.  So ntdll now has a stub for every Windows 10 1903
service, laid out in service-number order (one per line in
`userland/ntdll/ntdll.c`, from `nt1903_services.h`), the way Windows'
ntdll is; the services that used to be ntdll C code are now system calls
the kernel answers (`kernel/um/um_services.c`), and the kernel reads a
system-call number the way Windows does, from the low bits of `EAX`,
ignoring the noise Hyperion leaves in the top bits.

With that, every raw system call Hyperion makes lands on the service it
meant, and it gets past the point where it used to crash on service 0: it
initialises, reads the kernel's loaded-module list
(`NtQuerySystemInformation(SystemModuleInformation)`), its own token
(`NtQueryInformationToken`) and ntdll's version resource, and then stops
with "Roblox encountered an unexpected error" and exits on its own (exit
code 50) rather than crashing.  What it checks next is a deeper
Hyperion-specific integrity step, the faithful next piece of work.  NovaOS
does not, and will not, work around anti-cheat checks or change Roblox
itself.  `syscalltest` checks the table the way Hyperion reads it: it
ranks ntdll's `Zw` exports, confirms the numbers against Windows 10 1903
and that the kernel answers them.

## WebView2

Roblox's login page, and many other programs, show web content with
Microsoft Edge WebView2, a runtime Windows installs once for every
program.  Its installer is Microsoft Edge Update, a 32-bit program that
installs itself, registers its update tasks with the Task Scheduler and
its COM servers, and then runs the runtime's own setup.  On NovaOS, Edge
Update now starts and reaches its install step (NovaOS gained the
functions it calls: Task Scheduler 2.0, the Data Protection API,
`UrlCombine`, the package-name functions, the MDM enrolment check and
others).  It stops there: before installing anything it makes an MSXML 6
`DOMDocument`, which NovaOS does not have yet, and then reports that
Windows needs an update.  What comes after MSXML, in order: Edge Update
hands the install to its own COM server in another process (NovaOS's COM
is in-process only so far), the 32-bit updater's registry keys need
Windows' `WOW6432Node` view for the 64-bit programs that look for the
runtime there, and then the runtime itself (a Chromium browser process
with its sandbox) has to run.

## Steam

Steam's installer (`SteamSetup.exe` from steampowered.com, a 32-bit NSIS
installer) installs the bootstrapper in `C:\Programs\Steam`.  On its first
start the bootstrapper downloads the client from Valve's servers, first the
32-bit one and then the 64-bit one it updates itself to, unpacks the
packages, checks every file and starts the 64-bit client, which starts
`SteamService.exe` and its browser, `steamwebhelper.exe` (Chromium 126 in
`bin\cef\cef.win64`).  Steam picks the proxy from the user's Internet
Settings, as on Windows.

The login window does not come up yet: the browser process starts, opens
its threads and then never starts its GPU and page processes, so no window
appears.  What is known to be missing on the way, each a NovaOS gap and
none a reason to change Steam:

- `SteamService.exe` reports "Invalid file signature": it checks Steam's
  files with `WinVerifyTrust`, and NovaOS's `wintrust` does not check
  Authenticode signatures yet.  The faithful fix is Authenticode in
  `wintrust` (the signature in the certificate table, checked against the
  certificate store), not a service that says yes.
- Steam's service pipe ("Failed to create Service pipe") and its
  security descriptors in SDDL form (`advapi32`'s SDDL functions are
  incomplete).
- `GetAdaptersAddresses` reports no adapters, and the browser's UDP and
  TCP sockets fail with `WSAENOBUFS` under load (NovaOS's network code).
- DirectWrite's GDI interop (`CreateBitmapRenderTarget`), which Chromium
  draws text with.

How to check the rest by hand, which needs a Steam account and so is not
in the corpus: start Steam from the App Store, sign in (or pick "Go
offline" once signed in), install a small free game and
start it to its main menu.

## Programs that come with NovaOS

Built in: the Terminal, File Explorer, Notepad, Settings, Calendar,
Photos, the App Store, Install NovaOS, NovaOS's own `cmd.exe` and its
command-line tools, and the NetSurf web browser.  The [user guide](user-guide.md) describes
them.

## Reporting a program

If a program fails, open an issue on
[GitHub](https://github.com/dean-plude/os/issues) with its name, version
and download link, what you did and what happened, and the boot log
(the serial output in QEMU, or `EFI\NOVA\bootlog.txt` on a USB stick).
