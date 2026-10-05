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
| **Windows Installer (`.msi`) and NSIS setup programs** | Works | 7-Zip, CMake, Node.js, Temurin and KeePassXC packages install, make shortcuts and uninstall, custom actions included; 32-bit packages put their system files in `SysWOW64`; bootstrappers such as WiX Burn query products, patches and source lists; NSIS installers and their uninstallers run | core self-tests (`msitest`, `msiscript`, `msiqtest`), by hand |
| **Notepad++ 8.8.3** (x64 portable) | Works | Opens, edits and saves files through the Open and Save As dialogs | nightly corpus |
| **SumatraPDF 3.4.6** (x86 portable) | Works | Opens a PDF and renders its pages; printing finds no printer | nightly corpus |
| **WinMerge 2.16.50** | Works | Compares two files side by side | nightly corpus |
| **PuTTY 0.81** | Partly | Raw connections work; SSH is untested | nightly corpus |
| **KeePassXC 2.7.12** (Qt 5) | Works | Opens and unlocks a KDBX 4 database and shows its entries; Windows Hello unlock reports "not supported" | nightly corpus |
| **VLC 3.0.21** (Qt 5) | Partly | Plays an H.264 and AAC MP4 with sound in its window; closing VLC can freeze NovaOS (being fixed) | nightly corpus |
| **Audacity 3.7.4** (wxWidgets) | Partly | Records from the microphone, draws the waveform and saves an `.aup3` project | nightly corpus |
| **Inkscape 0.91** (GTK 2) | Works | Opens, edits and saves an SVG; Inkscape 1.x is untested | nightly corpus |
| **Krita 5.3.4** (Qt 5) | Partly | Starts and opens a new image on Mesa 3D (install Mesa 3D from the App Store first); painting and saving are not checked yet | nightly corpus |
| **Firefox 157** and **Floorp 12.19** | Works | Installs from the App Store, loads pages over HTTP and HTTPS, scrolls and takes typing in forms; a publicly trusted HTTPS site is untested (the test network is offline) | nightly corpus |
| **Roblox** (App Store, the current client) | Partly | Its installer runs: it fetches Roblox's settings and packages over HTTPS and installs the client; its Hyperion anti-cheat now passes the system-call, alignment-fixup, 32-bit-code and thread-context checks and then reports "Virtual Machine detected" under QEMU, as Roblox does in any virtual machine (untested on a real PC) ([Roblox and anti-cheat](#roblox-and-anti-cheat)); the WebView2 runtime it sets up for logging in does not install yet ([WebView2](#webview2)) | nightly corpus (the installer) |
| **Microsoft Edge WebView2 runtime** (the evergreen offline installer) | Partly | Its installer, Microsoft Edge Update, installs itself, accepts Microsoft's signature on the runtime's package and starts the runtime's own setup, which installs the runtime; a WebView2 host starts the runtime's browser process (`msedgewebview2.exe`) and its GPU, network and storage processes and gets a WebView2 environment, but no page yet ([WebView2](#webview2)) | nightly corpus (the installer, `wv2host`) |
| **Mesa 3D 24.2.4** (App Store, Runtimes) | Works | OpenGL 4.5 on the CPU (llvmpipe) and Vulkan (lavapipe), 64- and 32-bit | CI graphics tests (`gltest`) |
| **DXVK 2.5.3** (App Store, Runtimes) | Works | Direct3D 8 to 11 on Vulkan, 64- and 32-bit; ANGLE's Direct3D 11 back end (Chromium's GPU process: Steam's browser, WebView2, Qt WebEngine) starts on it | CI graphics tests (`d3dtest`, `d3dtest angle`) |
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
| **Visual C++ Redistributable** (App Store) | Works | NovaOS has its own `vcruntime140` and `msvcp140`, so most programs do not need it, but GOG GALAXY needs its MFC.  Its installer (WiX Burn) installs the Minimum and Additional Runtimes, the 32-bit one into `SysWOW64` and the 64-bit one into `System32` (14.44 x86 and 14.51 x64 checked), replacing NovaOS's own copies there | by hand |
| **Microsoft Build of OpenJDK 21** (App Store) | Untested | Temurin 21 (above) works | — |
| **OpenTTD 15.3** (App Store; free on GOG; the corpus installs OpenTTD's own Windows installer, as GOG's copy needs an account to download) | Works | Its setup program installs it silently (run as administrator, as its manifest asks); with the OpenGFX graphics it reaches its main menu, and its animated title game plays, drawn in software (no hardware acceleration yet) | nightly corpus |
| **GOG GALAXY 2.1** (offline installer, Inno Setup 6) | Partly | Its setup program runs elevated, through its wizard or silently (`/VERYSILENT`), installs the Visual C++ runtimes it carries (x86 and x64), copies GOG GALAXY's files and makes its shortcuts; the client starts (`GalaxyClient.exe`, 64-bit, Qt 6 WebEngine: every import resolves, 89 modules, and the Windows Runtime `UISettings` its Qt plugin reads the theme from), installs and starts its service (`GalaxyClientService.exe`, which trusts the client and answers its requests over 127.0.0.1:9978), opens its window and warns that drive C: is not NTFS; past that warning it finishes initialising, opens its sign-in window and starts its first Chromium renderer (`QtWebEngineProcess.exe`), which needs the App Store's Mesa 3D (Qt then draws with its OpenGL; without it Qt ends the client: "Could not get handle for shared context") and DXVK (Chromium's Direct3D 11, through ANGLE); with both, Chromium's compositor stops because DXVK on lavapipe cannot share Direct3D 11 textures between devices. Setup warns that drive C: is not NTFS (answer Yes) | by hand |
| **Steam** (App Store, the current client) | Partly | Its installer runs and installs the bootstrapper; on first start Steam downloads its client over HTTPS, updates itself to the 64-bit client, verifies it and starts it with `SteamService` (which runs as a real service) and its browser, `steamwebhelper.exe` (Chromium); the browser starts its GPU, network, storage and page processes but does not open the login window yet ([Steam](#steam)). Signing in and games are unchecked | nightly corpus (install and update) |
| **Chocolate Doom 3.1.0** (32-bit, SDL2) with **Freedoom** | Works | Plays in a window on Mesa 3D's OpenGL (install "Mesa 3D" from the App Store first) in the right colours; an Xbox or HID game pad walks, turns and fires | by hand in QEMU; graphics self-test `gltest colors` |
| **Beneath a Steel Sky** on **ScummVM 2026.3** (App Store; free on GOG, which ships it with ScummVM; the corpus takes the freeware floppy release Revolution and ScummVM publish, as GOG's copy needs an account to download) | Works | ScummVM's setup program (Inno Setup, 32-bit, installing the 64-bit ScummVM) installs it silently or through its wizard; the game starts, Esc skips the intro and a click walks Robert Foster along the first scene's gantry. ScummVM draws with OpenGL 1.1, NovaOS's own when no OpenGL driver is installed; sound not checked yet (the corpus runs without a sound card) | nightly corpus |
| **Teeworlds 0.7.5** (App Store; free and open source, SDL2) | Partly | Installs from the App Store and starts in full screen on Mesa 3D's OpenGL (install "Mesa 3D" first: it needs OpenGL 1.2), plays its menu music through the sound card and goes through its first-start questions to its start menu with the keyboard, and its menus follow the mouse with its default settings (SDL reads the pointer's moves and recentres it with `SetCursorPos`; with `inp_grab 1` it reads Raw Input instead; the corpus opens Settings with a click). Joining a game on its own server times out without KVM, while Mesa compiles its shaders | nightly corpus |
| **OpenTyrian 2.1.20260913** (App Store; Tyrian 2.1, freeware since 2004, on the free and open-source OpenTyrian engine; SDL2) | Works | Installs from the App Store (the official 64-bit zip, which carries the freeware game data) and draws with Direct3D 9, SDL's first choice on Windows, through DXVK on Mesa 3D's Vulkan (install "Mesa 3D" and "DXVK" first). Its demo plays in a window, Alt+Enter switches it to full screen (the Direct3D 9 device is reset at the display's size), its music plays through the sound card, and the keyboard takes it through its menus into a new game. Full screen is SDL's desktop full screen, so the display mode does not change | nightly corpus |
| **Blobby Volley 2 1.1.1** (App Store; free and open source, GPL; 32-bit, SDL2) | Works | Installs from the App Store (the official zip) and plays in an 800x600 window. Its Fullscreen option is a real display-mode switch: SDL changes the display to 800x600 (`ChangeDisplaySettingsEx`) and draws with Direct3D 9 in exclusive full screen through DXVK on Mesa 3D's Vulkan (install "Mesa 3D" and "DXVK" first; without them it draws with SDL's software renderer); the display goes back to its own mode when the game ends | nightly corpus |
| **LBreakout2 2.6.5** (App Store; free and open source, GPL; 64-bit, SDL 1.2) | Works | Installs from the App Store (the official zip) and plays in a 640x480 window, drawn with GDI (SDL 1.2's `windib` driver: a 16-bit DIB section blitted with `BitBlt`). Its full screen ('f' anywhere) switches the display to 640x480 with `ChangeDisplaySettings` and takes the frame off its window (`WS_POPUP`); 'f' again, or ending the game, puts the display and the frame back | nightly corpus |

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
(`NtQueryInformationToken`) and ntdll's version resource.

Hyperion then checks a run-time behaviour of the kernel: it turns on
alignment-fault fixup and runs a deliberately misaligned `MOVDQA` in a
page it has just allocated, expecting the move to go through rather than
the process to die.  Windows' kernel, when fixup is on, rewrites a
misaligned 16-byte SSE move to its unaligned form in the program's code
and retries it; NovaOS now does the same
([the history note](history/2026-10-04-alignment-fault-fixup.md),
`kernel/um/um_gpfault.c`), along with the smaller things the same code
path checks: `ThreadHideFromDebugger` validating its length and ntdll's
extended-context (XSAVE) functions.  Past that, Hyperion runs further into
its start-up — hundreds more system calls, reading registry keys and the
system's time-zone information.

Next it runs 32-bit code inside the 64-bit client: it maps memory just
below 4 GiB, puts a `CPUID` in its last two bytes and far-jumps there
through selector 0x23, Windows' 32-bit user code segment, so the
instruction pointer wraps to 0 and faults, and its handler looks at what
the fault says.  NovaOS used its own selector numbers, where 0x23 was the
64-bit code segment, so the "32-bit" code ran as 64-bit code and the
client crashed.  NovaOS now has Windows' x64 segment layout (0x23 32-bit
code, 0x2B data, 0x33 64-bit code, 0x53 the 32-bit TEB;
[the history note](history/2026-10-04-windows-segment-layout.md)), a fault
in such code reaches the program's handlers with `SegCs` 0x23 and the
handler resumes in 64-bit code.  Hyperion then reads its own registers
with `NtGetContextThread` on itself, which NovaOS now answers as Windows
does (`SetThreadContext` on the calling thread works too, and
`__fastfail` ends a program the Windows way).

Past those checks Hyperion reads the firmware's SMBIOS table and the
display devices and stops with **"Virtual Machine detected. Roblox can't
be used in a Virtual Machine or Virtual Desktop."**  That is the right
answer under QEMU, where NovaOS's tests run: Roblox refuses virtual
machines on Windows too, and NovaOS reports the machine it runs on
truthfully.  The next Roblox steps need a real PC (the ThinkPad T14 of
[Phase 21](hardware.md)); on one, this check should pass, but it is
untested.  NovaOS does not, and will not, work around anti-cheat checks,
hide the virtual machine or change Roblox itself.  `syscalltest` checks the
table the way Hyperion reads it, `aligntest` the alignment-fault fixup and
`gatetest` the segment layout, the 32-bit code, the calling thread's
context and `__fastfail`.

## WebView2

Roblox's login page, and many other programs, show web content with
Microsoft Edge WebView2, a runtime Windows installs once for every
program.  Its installer is Microsoft Edge Update, a 32-bit program that
installs itself, registers its update tasks with the Task Scheduler and
its COM servers, and then runs the runtime's own setup.  On NovaOS, Edge
Update now starts and reaches its install step (NovaOS gained the
functions it calls: Task Scheduler 2.0, the Data Protection API,
`UrlCombine`, the package-name functions, the MDM enrolment check and
others), reads its manifests with MSXML 6 and installs itself.  Its
`/regserver` step registers its proxy/stub DLL (`psmachine.dll`) through
rpcrt4's NDR engine, and COM calls between processes work (ole32's
standard marshaler over named pipes, `LocalServer32` servers started on
demand, oleaut32's `IDispatch` proxy and `BSTR`/`VARIANT` marshaling).
Its silent install unpacks the runtime's package and checks that
Microsoft signed it: `WinVerifyTrust`, then crypt32's Microsoft root
chain policy (`CERT_CHAIN_POLICY_MICROSOFT_ROOT`, with the application
root flag for Microsoft's 2011 root), which NovaOS now answers as
Windows does, so the package is accepted and cached.  Edge Update's own
background update pass (`/ua`), which runs alongside the install, finds
the install worker by listing Edge Update's processes and reading each
one's image path (`GetProcessImageFileName`), owner and command line
(from its PEB); NovaOS now answers those questions about another
process, so the pass leaves the install alone, and the install starts
the runtime's own setup (`MicrosoftEdgeWebview_X64_*.exe --msedgewebview
--user-level`, Chromium's `mini_installer` with `setup.exe`).  That setup
unpacks its 728 MB archive (`MSEDGE.7z`) through a file mapping, which
NovaOS used to refuse over 256 MB, reports to Windows Error Reporting
through `wer.dll` (NovaOS answers as a machine with reporting turned
off) and checks that it runs on a desktop (`RtlGetDeviceFamilyInfoEnum`).
It then copies the runtime into
`C:\AppData\Local\Microsoft\EdgeWebView\Application` and gives the
runtime's sandboxed processes read access to that folder: it reads the
folder's permissions, adds one entry and sets them back.  Drive C: used
to have no permissions list anywhere (as on FAT), so that one entry
became the folder's whole list and shut the user out of it ("Access is
denied" on `SetupMetrics`, and the install rolled back).  C:'s root now
has the permissions Windows gives `C:\` (and that a new NTFS volume's
root already had), which every folder inherits, so the added entry joins
them and the setup finishes: the runtime is installed and Edge Update
records it.  Its installer then cleans up its temporary files, marking
each for deletion on close; NovaOS deletes such a file when its last
handle closes, as Windows does.

The runtime itself now starts.  `wv2host`, a small WebView2 host NovaOS
builds, loads `WebView2Loader.dll` from Microsoft's WebView2 SDK out of
its own folder (NovaOS now searches the program's current folder for
DLLs after the system folders, as Windows' safe search order does),
finds the installed runtime and creates a WebView2 environment: the
browser process (`msedgewebview2.exe`) starts with its GPU, network and
storage processes.  For that NovaOS publishes COM's apartment in the
thread's TEB (`ReservedForOle`, which Chromium reads instead of asking
COM), answers the functions the browser calls (`GetAddrInfoExW`,
`RtlIpv4StringToAddressEx` and its IPv6 form, `LdrLockLoaderLock`,
`CryptFindOIDInfo`, the performance counter provider API, power
notifications, the packaged-app queries, `GetDllDirectory`), gives each
process up to 4080 fiber-local storage slots, ends a process that calls
`TerminateProcess` on itself without running DLL detach (Chromium's
DllMain deliberately crashes on a detach it does not expect), and its
`__C_specific_handler` and `RtlUnwindEx` no longer run a `__finally`
twice when an exception is caught by an `__except` inside it (the
Microsoft C++ runtime re-raises a rethrow from such a block, and
`oneauth.dll` aborted the browser on it).  The browser then connects
to the host's pipe and checks that the pipe's server is the host
(`GetNamedPipeServerProcessId`); NovaOS's pipes now keep both ends'
process ids, so it keeps the connection and makes the WebView's window
(it also needed shlwapi's ordinal 14, `GetAcceptLanguagesA`).  The host
then moves that window into its own with `SetParent` and `SetWindowPos`;
NovaOS's window handles now work across processes (messages, the calls
on a window, `SetParent` of another process's window and drawing into
it, checked by `xpwin`), so the controller is made, the page loads and
a script runs in it (`wv2host: script "NovaOS WebView2 / 42"`), with
the event log functions the browser calls afterwards (`EvtSubscribe`,
bookmarks, logs).  The page is drawn: the GPU process finds no
Direct3D 11 renderer for ANGLE (none without DXVK; NovaOS's WARP device
does not rasterize, so ANGLE refuses it) and falls back to Chromium's
software compositor, which draws through a DXGI 1.2 swap chain on a WARP
device shown by DirectComposition in a window the GPU process makes and
the browser parents in its own.  NovaOS's `dxgi.dll` now lists Windows'
Basic Render Driver, `d3d11.dll` gives that WARP device, `dcomp.dll`
draws a visual's swap chain into its window, and a child window parented
by another process becomes a window of its own kept inside the new
parent (`dcomptest`).
64-bit programs that look for a machine-wide
runtime under Windows' `WOW6432Node` registry view also need that view
(the per-user install records itself under `HKEY_CURRENT_USER`, which
has none).

## Steam

Steam's installer (`SteamSetup.exe` from steampowered.com, a 32-bit NSIS
installer) installs the bootstrapper in `C:\Programs\Steam`.  On its first
start the bootstrapper downloads the client from Valve's servers, first the
32-bit one and then the 64-bit one it updates itself to, unpacks the
packages, checks every file and starts the 64-bit client, which starts
`SteamService.exe` and its browser, `steamwebhelper.exe` (Chromium 126 in
`bin\cef\cef.win64`).  Steam picks the proxy from the user's Internet
Settings, as on Windows.

The login window does not come up yet.  The browser now starts its child
processes as on Windows: the GPU process, the network and storage
services and a page (renderer) process for Steam's first page.  The GPU
process draws with ANGLE on Direct3D 11 when Mesa 3D and DXVK are
installed; without them it gives up after three tries and Chromium draws
in software, as it does on Windows without a usable GPU.  The browser
used to close a few minutes in because `steam.exe` itself stopped: its UI
imports `AcceptEx` from `wsock32.dll` (ordinal 1141), which NovaOS's
Winsock 1.1 library did not export, so the first connection it accepted
ended the client with `STATUS_ENTRYPOINT_NOT_FOUND` and the browser shut
down with it.  `wsock32` now exports `TransmitFile`, `AcceptEx` and
`GetAcceptExSockaddrs` (1140 to 1142) from `mswsock`, as Windows does,
and Steam keeps running.  The browser then crashed in DirectWrite:
Chromium asks the DirectWrite factory for `IDWriteFactory2` and
`IDWriteFactory3` (Windows 10's font sets, face references and font
fallback) without checking the answer.  NovaOS's DirectWrite now has
them, and the browser keeps running: under emulation it starts its GPU,
network and storage processes, creates Steam's first browser ("SP Shared
JS Context") and launches its page process about eight minutes in, which
is as far as the corpus test's fifteen minutes reach.  `steam.exe` also
asks for `IDWriteFactory5` and goes on without it.  Run longer, the
browser restarted every few minutes: its network process got Winsock
error 10038 from `connect` on a UDP socket (Chromium's DNS client), which
NovaOS's kernel did not support, it hands sockets between processes with
`WSADuplicateSocket`, which was missing, and the browser process stopped
on a check that a file handle it passes to a child is read-only
(`NtQueryObject` reported every handle as holding all rights).  All three
are fixed; the browser no longer stops, and about half an hour in Steam
opens its first window of its own, an "Unexpected Transport Error"
dialog, drawn by the browser but with its text missing.  Its network
process still ends and restarts every minute or two, which is the next
thing to find.
What is known to be missing on the way, each a NovaOS gap and none a
reason to change Steam:

- Media Foundation (`mf.dll`, which Chromium only uses for video).
- DirectWrite's `IDWriteFactory4` and later (`steam.exe` asks for
  `IDWriteFactory5` and goes on without it).
- WMI (`WbemLocator`, `{4590F811-1D3A-11D0-891F-00AA004B2E24}`), which
  Steam and its browser ask for and get on without (Chromium reads the
  board and BIOS names through it), and the COM classes
  `{33C53A50-F456-4884-B049-85FD643ECFED}`,
  `{E77CC89B-7401-4C04-8CED-149DB35ADD04}` and
  `{E2B3C97F-6AE1-41AC-817A-F6F92166D7DD}`, which it gets on without.
- `SteamService.exe` checks Steam's files with `WinVerifyTrust` and
  accepts them: NovaOS checks Authenticode signatures (the digest, Valve's
  chain to DigiCert's root and the timestamp), offline, so a revocation
  check it is asked for fails rather than passes.  The service then
  updates itself once, then runs as a real service under the control
  manager (`StartService` no longer fails with
  `ERROR_SERVICE_REQUEST_TIMEOUT`, 1053, now that `CopyFile` keeps a
  file's last-write time the way Windows does: the service compares it to
  tell whether its copy is current), installs its helper files and keeps
  running.  It asks for an event-tracing (ETW) session to watch process
  starts; NovaOS runs no trace sessions, so `StartTrace` fails as it does
  on Windows when no session can start, and the service watches processes
  without one.
- Steam's service pipe ("Failed to create Service pipe") and its
  security descriptors in SDDL form (`advapi32`'s SDDL functions are
  incomplete).
- `GetAdaptersAddresses` reports no adapters.  (The browser's sockets
  and Steam's downloads no longer fail with `WSAENOBUFS`: NovaOS's socket
  tables were sized for a small device; see `loadtest`.)
  Chromium's network change and DNS watchers do not start
  (`WSALookupServiceBegin` has no providers, error 10108).
- DirectWrite's GDI interop (`CreateBitmapRenderTarget`), which Chromium
  draws text with.
- The "Thread creation failed" messages `steam.exe` logged once under
  load did not come back in about three hours of runs; NovaOS logs a
  refused thread (`no new thread`) and never did.

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
