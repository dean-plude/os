# NovaOS — Feature History, Phase by Phase
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

This is the detailed record of what each phase of NovaOS added, in the
order it landed.  The [README](../README.md) has the short version: what
runs today, how to build it, and where the project is going.

Each section describes the system as that phase left it.  Where a "Not
yet" item was done by a later phase, a note says which; everything else
under "Not yet" is still open (see [ROADMAP.md](ROADMAP.md)).

Contents:
[1 Boot](#phase-1--boot--kernel-foundation) ·
[2 NT kernel](#phase-2--nt-kernel-personality) ·
[3 I/O and PE loader](#phase-3--io-sections-and-pe-loader) ·
[4 VFS](#phase-4--vfs--initrd) ·
[5 Isolation](#phase-5--process-isolation--user-mode-foundation) ·
[6 User mode](#phase-6--full-user-mode-foundation) ·
[7 GUI](#phase-7--gui-window-manager--desktop-shell) ·
[8 Desktop](#phase-8--interactive-desktop) ·
[8 Networking](#phase-8-cont--networking) ·
[9 Windows programs](#phase-9--windows-programs-ring-3) ·
[9.5 NetSurf](#phase-95--the-netsurf-web-browser) ·
[Desktop UX](#desktop-ux-refresh) ·
[10 DLLs, registry, COM, storage](#phase-10--standard-dlls-registry-com-and-persistent-storage) ·
[11 SMP](#phase-11--multiprocessor-smp) ·
[12 7-Zip](#phase-12--windows-gui-programs-7-zip-unmodified) ·
[App Store](#the-app-store) ·
[Windows Installer](#windows-installer-msi-packages) ·
[13 WoW64](#phase-13--32-bit-windows-programs-wow64) ·
[Shortcuts](#shortcuts-lnk-and-overlapping-controls) ·
[Pipes and cmd.exe](#pipes-and-cmdexe) ·
[Clipboard](#the-clipboard) ·
[Git](#git) ·
[MSYS2](#the-msys2-runtime-shexe-clone-push) ·
[Runtimes](#language-runtimes-java-net-nodejs-python) ·
[Installing NovaOS](#installing-novaos-on-a-disk) ·
[Sound](#sound-intel-hd-audio-winmm-and-wasapi)

<!-- BEGIN generated:history -->

## Phase 1 — Boot & Kernel Foundation
- UEFI bootloader (PE32+ EFI application, loads kernel ELF from FAT32 ESP)
- Physical memory manager (bitmap allocator, reads UEFI memory map)
- Virtual memory (4-level paging, 64 GiB physmap, NX via EFER.NXE, CR4.PGE)
- Kernel heap (slab allocator: kmalloc/kfree/kzalloc)
- GDT/TSS with IST stacks for NMI, double fault, machine check
- IDT with all 256 handlers (CPU exceptions, APIC IRQs, INT 0x2E syscall gate)
- APIC timer at 100 Hz (legacy 8259A PIC disabled)
- Preemptive round-robin kernel scheduler (kernel threads, context switch via callee-saved regs)
- Serial console (COM1, 115200 baud) + GOP framebuffer text output
- Boots in QEMU with OVMF (UEFI firmware)

## Phase 2 — NT Kernel Personality
- Object Manager (`ObCreateObject`, `ObReferenceObject`, `ObDereferenceObject`, handle tables)
- Security Reference Monitor (`SeInitialize`, `SeCreateSystemToken`, ACL/ACE stubs)
- Process Manager (`EPROCESS`/`ETHREAD`/`KPROCESS`/`KTHREAD`, `PsCreateSystemProcess`, `PsCreateSystemThread`)
- Configuration Manager (registry hive bootstrap, `CmInitialize`)
- Syscall dispatcher: INT 0x2E (legacy) + SYSCALL/SYSRET MSR path (`KiSystemCall64`)
- NT syscall table with ~40 entries (NtAllocateVirtualMemory, NtCreateFile, etc.)

## Phase 3 — I/O, Sections, and PE Loader
- I/O Manager (`IoCreateDevice`, `IoCreateFile`, IRP dispatch: Create/Read/Write/QueryInfo)
- Virtual Memory Areas (`VMA_SPACE`, `VmaAllocate`, `VmaFree`, `VmaProtect` per-process)
- Section Objects (`MmCreateImageSection`, `MmMapImageView`, anonymous sections)
- PE32+ Loader (`LdrLoadImage`): header validation, section mapping, base relocation, import resolution
- Built-in stub DLLs: ntdll, kernel32, msvcrt, user32 (syscall thunks for import resolution)
- User-mode process creation (`PsCreateUserProcess` → IRETQ into ring-3 via `PsUserThreadEntry`)

## Phase 4 — VFS & InitRD
- VFS abstraction layer (`VfsMount`, `VfsOpen`, `VfsRead`, `VfsReadDir`, `VfsStat`)
- NT path alias resolution (`\??\C:\`, `\SystemRoot`, `\DosDevices\C:` → first mount)
- CPIO newc InitRD driver (`InitrdMount`): parses in-memory CPIO archive, mounts as `\Device\InitRD`
- Boot protocol v2: bootloader passes `initrd_base`/`initrd_size` in `BootInfo`
- File I/O via IRP dispatch: `NtReadFile`, `NtWriteFile` fully wired through VFS to InitRD

## Phase 5 — Process Isolation & User-Mode Foundation
- Per-process page tables: each user process gets its own PML4 (`paging_create_process_pt`)
- User-space mappings snapshotted into process PML4 at creation (`paging_clone_user_mappings`)
- CR3 switch on every context switch in the scheduler (`perform_switch`)
- KPCR (Kernel Processor Control Region): per-CPU GS-relative structure
  - `KPCR.KernelRsp` holds the current thread's kernel stack top for the SYSCALL path
  - Updated by the scheduler on every context switch
- SWAPGS: on SYSCALL entry, GS → KPCR; on SYSRET, GS → user TEB
- Kernel stack switch: `syscall_entry.asm` loads `gs:[KPCR_KERNEL_RSP]` before entering C
- PEB64/TEB64 allocation: `PsAllocatePebTeb` maps one page each at fixed user VAs
  - TEB.NtTib.Self initialized (GS:[0x30] self-pointer)
  - TEB.ClientId (PID/TID) and PEB.ImageBaseAddress set
- User GS base (`MSR_GS_BASE`) set to TEB VA before IRETQ so SWAPGS works on first syscall

## Phase 6 — Full User-Mode Foundation
- **User-mode SYSCALL thunk pages** (`ldr/user_stubs.c`): one 4 KiB page per stub DLL
  (ntdll, kernel32, msvcrt, user32) containing genuine ring-3 x86-64 machine code:
  `mov r10,rcx; mov eax,N; syscall; ret` (16-byte slots)
  - Mapped read+exec into every new process's private page table
  - IAT entries now hold user-mode VAs — imported functions callable from ring-3
- **Kernel-helper syscalls** (0x01F0–0x01FF): extend the NT dispatch table for Win32
  helpers with no NT syscall number:
  - `KH_RtlAllocateHeap/FreeHeap/ReAllocateHeap` → `VmaAllocate`/`VmaFree` on process VMA
  - `KH_GetCurrentProcessId/ThreadId` → read from EPROCESS/ETHREAD
  - `KH_GetLastError/SetLastError`, `KH_IsDebuggerPresent`, `KH_GetStdHandle`
  - `KH_DbgPrint`, `KH_RtlInitUnicodeString`, `KH_RtlZeroMemory/MoveMemory`
  - `KH_GetCurrentProcess` → `(HANDLE)-1`, `KH_GetCurrentThread` → `(HANDLE)-2`
- **New NT syscalls**: `NtSetInformationThread`, `NtFlushInstructionCache`,
  `NtCreateProcessEx`, `NtCreateThread` (stubs; full implementation Phase 7)
- **`KiSystemCallDispatch` now returns `UINT64`** so kernel-helper calls can return
  64-bit pointers (e.g., heap allocations) directly in RAX
- **PEB improvements**: `ProcessHeap`, `NtGlobalFlag`, `CriticalSectionTimeout`,
  `HeapSegment*`, `MaximumNumberOfHeaps`, `CurrentLocale` all initialized
- **CSRSS bootstrap** (`ps/csrss.c`): kernel-side shim that:
  - Runs a `csrss` kernel thread (event loop placeholder)
  - `CsrRegisterProcess()` — logs and tracks every new user process
  - `CsrClientCallServer()` — no-op stub returning `STATUS_SUCCESS`
  - Prevents ntdll's `LdrpInitializeProcess` from failing on CSRSS connect

## Phase 7 — GUI, Window Manager & Desktop Shell
- **GDI software renderer** (`gdi/gdi.c`): a dependency-free 2D rasterizer that
  draws straight into the GOP linear framebuffer (BGR/RGB aware, fully clipped):
  - Solid / alpha-blended fills, vertical & horizontal gradients
  - Rounded rectangles (solid, alpha, vertical-gradient), circles, H/V lines
  - Color lerp, and text via the embedded 8×16 VGA font
    (normal, bold double-strike, transparent, centered)
- **Window manager** (`wm/wm.c`): a compositing WM over GDI:
  - Fixed pool of `WND` windows with Z-ordering, focus, show/hide
  - Decorated frames — title bar, accent stripe, caption glyphs, drop shadow,
    rounded body and 1px border
  - `WmComposite()` paints background → windows (ascending Z) → overlay
- **Desktop shell** (`wm/desktop.c`): a Windows-11-style desktop, rendered fully
  in software to match the design mock:
  - Gradient wallpaper with six warm "wave" layers (integer-sine curves)
  - Left-column desktop icons (My PC, Documents, Personal, Proyect, Files, photos)
  - Centered **Start menu**: search box, pinned-app grid, "see all" pill, live
    tiles (Today/calendar, weather, media, cloud storage, photos, to-do, games),
    a "Recently used" list, and a user / power bar — with two app tiles peeking
    above the panel
  - Floating, rounded **taskbar/dock** with app glyphs and a clock
- **Framebuffer raw-surface API** (`fb_get_raw`, `fb_draw_string[_trans]`): lets
  GDI write pixels directly; `kprintf_set_fb_enabled(false)` silences the boot-log
  text console once the desktop is painted (serial keeps every message)
- Booted as **STEP 22** in `KiSystemStartup`: after the executive is up the kernel
  initializes GDI → WM → shell and renders one full desktop frame

## Phase 8 — Interactive Desktop
- **Preemptive multitasking fixed**: the APIC timer ISR now calls `sched_tick()`
  (it was a stubbed `TODO`), so created threads actually run and are
  time-sliced. The shell runs in its own `desktop` kernel thread.
- **PS/2 input** (`hal/ps2.c`): polled i8042 keyboard + mouse; bytes are decoded
  into an event queue (`wm/input.c`). Keyboard uses scancode set 1; the mouse
  uses the 3-byte streaming packet.
- **Software mouse cursor** (`wm/wm.c`): an arrow drawn with *save-under* so it
  moves without recompositing the whole 2560×1600 scene.
- **Event loop** (`DesktopRun`): polls input, drives the cursor, and recomposites
  only on state change or once per minute. Left-click hit-tests the Start button
  (toggles the menu) and click-away / **Esc** closes it.
- **Live clock** (`hal/rtc.c`): the dock clock + date are read from the CMOS RTC.
- Verified under QEMU+OVMF: cursor tracks the mouse, Esc closes the Start menu,
  and the clock advances in real time.

## Phase 8 (cont.) — Graphics & Working Desktop
- **HiDPI rendering** (`gdi/`): logical coordinates with an integer display
  scale (2× at 2560×1600); anti-aliased shapes, soft shadows, double
  buffering, and text in **Inter** / **Cascadia Mono** pre-rasterized at build
  time by `tools/mkfont.c` (fonts under `third_party/`, SIL OFL 1.1).
- **Window manager** (`wm/wm.c`): Windows 11-style frames; focus and z-order,
  drag by the title bar, double-click to maximize, minimize/maximize/close
  buttons, Alt+F4, keyboard focus, clipped client painting.
- **Built-in apps** (`apps/`): Terminal (a shell: `dir`, `cd`, `type`, `echo`,
  `mkdir`, `del`, `start`, `sysinfo`, `dmesg`, …), File Explorer, Notepad
  (edit + Ctrl+S), Settings (live system/display/storage info) and Calendar.
  Third-party apps pinned in the dock/Start menu open a "not available yet"
  note until real `.exe` files can run.
- **Drive C:** is a RAM disk (`fs/ramfs.c`) with starter folders and files;
  changes last until reboot.
- **Shell** (`wm/desktop.c`): the dock launches/focuses/minimizes apps and shows
  running indicators; Start menu tiles and "Recently Used" launch apps;
  desktop icons open on double-click; the Windows key toggles Start.
- Keyboard/mouse input is drained from the 100 Hz timer interrupt, so no
  input is lost while a frame is being drawn.
- Not yet real: apps are kernel-side callbacks (no user-mode GUI programs),
  there is no disk, and there are no image icons.  *(Since done: Windows
  programs in Phase 9, a disk in Phase 10, icons in the Desktop UX
  refresh.)*

## Phase 8 (cont.) — Networking
- **PCI** (`hal/pci.c`): bus scan through configuration mechanism #1; BAR
  decoding (32/64-bit) and bus-master enable.
- **Network card** (`drivers/e1000.c`): Intel e1000 (82540EM, QEMU `e1000`)
  and e1000e (82574L, the q35 default); 32-entry RX/TX descriptor rings,
  polled by the net thread.
- **TCP/IP** (`net/net.c`): [lwIP 2.2.0](https://savannah.nongnu.org/projects/lwip/)
  (BSD licence, vendored in `third_party/lwip`) running on a dedicated `net`
  kernel thread — Ethernet/ARP, IPv4, ICMP, UDP, TCP, **DHCP** and **DNS**.
  Apps start asynchronous operations (resolve, ping, HTTP GET) and poll them
  from their window's tick, so the desktop never blocks on the network.
- **Terminal tools**: `ipconfig`, `ping [-n N] host`, `nslookup host`,
  `curl URL` (prints the response) and `wget URL` (saves to `C:\Downloads`);
  HTTP/1.0 with up to 5 redirects; Ctrl+C cancels. **Settings → Network**
  shows the adapter, MAC, lease, gateway and DNS servers live.
- QEMU's default user-mode network works out of the box (guest `10.0.2.15`,
  host reachable as `10.0.2.2`); add `-nic user,model=e1000` to test the
  older card.
- **HTTP/1.1 client** (`net/http.c`): keep-alive connection pool — follow-up
  requests and redirects to the same server reuse the open connection (idle
  connections are kept 30 s; a request that meets a connection the server
  already closed is retried once); `Content-Length`, chunked and
  close-delimited bodies; truncated responses are reported as errors.
- **HTTPS** (`net/tls.c`): [Mbed TLS 3.6](https://www.trustedfirmware.org/projects/mbed-tls/)
  (Apache-2.0, vendored in `third_party/mbedtls`, configured by
  `net/port/mbedtls_nova_config.h`) — **TLS 1.3 and 1.2** (ECDHE, AES-GCM,
  ChaCha20-Poly1305), full X.509 chain validation (trust, expiry, host name
  or IP address), and **session resumption** (TLS 1.3 tickets / TLS 1.2
  sessions) for new connections to a server seen before.  The built-in
  store holds the **146 Mozilla root CAs** (`net/tls_roots.c`, regenerated by
  `scripts/gen-tls-roots.py`); `certutil` lists them and
  `certutil -addstore root <file>` trusts extra PEM/DER CAs until reboot.
  Random numbers: Mbed TLS's CTR-DRBG seeded from a SHA-256 entropy pool
  fed by RDRAND (when present), TSC jitter and packet timing.
- **Terminal**: `curl`/`wget` take several URLs (`curl URL URL ...`) and show
  the protocol, cipher suite, reused connections and resumed sessions.
- Not yet: IPv6 and HTTP/2.  (The web browser arrived in Phase 9.5; IPv6
  and HTTP/2 came with "IPv6, HTTP/2 and virtio-net (Phase 18.8)".)

## Phase 9 — Windows programs (ring 3)
- **Real PE32+ `.exe` files run in ring 3** (`kernel/um/`): each program has
  its own page table (kernel half shared), a PEB/TEB and
  `RTL_USER_PROCESS_PARAMETERS` (UTF-16 command line, environment, current
  directory, standard handles).  The loader maps the image and its DLLs from
  drive C: with relocations, imports, forwarders and per-section protections
  (NX for data); ntdll then runs each module's TLS callbacks and `DllMain`
  in dependency order, so real DLL initialization happens.
- **Threads and synchronization**: `NtCreateThreadEx` and a per-process
  thread table (each thread with its own stack, TEB, FPU/SSE state and GS
  base); kernel objects reached through handles — events (auto/manual
  reset), mutexes (recursive, abandoned on owner exit), semaphores and
  thread objects — with `WaitForSingleObject`/`WaitForMultipleObjects`.
  ntdll critical sections, SRW locks, condition variables, one-time init;
  kernel32 `CreateThread`, TLS/FLS slots and the `Interlocked*` intrinsics.
- **Static TLS**: `__declspec(thread)` data, `_tls_index`/`_tls_used`, per
  thread, with `TlsAlloc`/`TlsGetValue` slots.
- **Structured exceptions (SEH)**: the kernel delivers CPU faults to ntdll's
  `KiUserExceptionDispatcher`; ntdll implements the x64 unwinder
  (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`) and
  `__C_specific_handler`, so `__try`/`__except`/`__finally`, vectored
  handlers and `RaiseException` work (built with `-fasync-exceptions`, so
  hardware faults are caught).  An unhandled fault ends the process with a
  precise report; the rest of the system keeps running.
- **System DLLs built from source** (`userland/`, compiled with
  `clang --target=x86_64-pc-windows-msvc` + `lld-link` by
  `tools/build_userland.py`) and installed in `C:\Windows\System32`:
  - `ntdll.dll` — NT system-call stubs, the loader, `Rtl*` helpers, a heap;
  - `kernel32.dll` — files, directories, console, memory, environment, time,
    processes/threads, sync, dynamic loading; A (UTF-8) and W entry points;
  - `msvcrt.dll` — a C runtime (`stdio`, `stdlib`, `string`, `math`, ...);
  - `ws2_32.dll` — **Winsock 2**: `socket`/`connect`/`bind`/`listen`/
    `accept`/`send`/`recv`/`sendto`/`recvfrom`/`select`/`gethostbyname`/
    `getaddrinfo`, over lwIP (`kernel/net/sock.c`);
  - `user32.dll` + `gdi32.dll` — a **Win32 GUI**: window classes,
    `CreateWindowEx`, the `GetMessage`/`DispatchMessage` loop,
    `DefWindowProc`, painting (`BeginPaint`, `FillRect`, `TextOut`,
    `Rectangle`, `Ellipse`, pens/brushes), timers and mouse/keyboard input.
    Each program window is a real window in the desktop's window manager,
    drawn from a client bitmap the program owns (`kernel/um/um_gui.c`).
- **NT services** (`um/um_syscall.c` and friends): files, directories, file
  and volume information, virtual memory (256 MB per process), time, delays,
  thread/process/synchronization objects, sockets and windows.  Programs may
  reach only the implemented services and every user pointer is checked.
- **Terminal**: type a program's name — console programs (`hello`, `mandel`,
  `primes 2000000`, `wc ...`, `guess`) stream output and take input, with
  **Ctrl+C** and **Ctrl+Z**; GUI programs (`winhello`) open their own
  window; `netcat host path` fetches a URL over sockets.  `tasklist` and
  `taskkill /PID n` list and stop programs; several run at once.
- Sample and self-test programs live in `userland/programs/`
  (`crttest` 26/26, `filetest` 30/30, `sectest` 13/13, `threads` 37/37,
  `dlltest` 10/10, `posixtest` 28/28, plus `netcat` and `winhello`).
- Not yet: loading the real Microsoft DLLs and a full modal dialog manager.
  *(The dialog manager came in Phase 12; the system DLLs are still NovaOS's
  own.)*

## Phase 9.5 — The NetSurf web browser
- **[NetSurf](https://www.netsurf-browser.org/) 3.11** — a real HTML/CSS
  browser engine (libcss, libdom, hubbub) — built from source for NovaOS
  (`tools/build_netsurf.py`, clang + lld-link for x86_64-pc-windows-msvc)
  into `C:\Programs\NetSurf\netsurf.exe`, a Windows program that runs in
  ring 3 on the system DLLs.  Open it from the **globe icon in the dock**
  (or Start), or type `netsurf [url]` in the Terminal (its log then appears
  there).  It renders HTML and CSS 2.1 (floats, tables, positioning; web
  fonts are not loaded), PNG, JPEG, GIF and BMP images, and plain text;
  links, forms, cookies, history, Back/Forward/Reload and error pages work.
- **HTTP and HTTPS** (`userland/netsurf/fetch_nova.c`): NetSurf's fetcher
  interface implemented over Winsock (`ws2_32`) with Mbed TLS 3.6 in the
  program (TLS 1.3/1.2, SNI, ALPN, certificate verification against the
  Mozilla roots in `res\ca-bundle.der` plus any root added with
  `certutil -addstore root`, which the Terminal now also saves to
  `C:\Windows\System32\CertStore`).  Each fetch runs on its own thread;
  responses may be chunked and gzip/deflate compressed; redirects, 304,
  401 (Basic auth), POST (url-encoded and multipart) and cookies are
  handled.  Random numbers come from the kernel's entropy pool through a
  new `NtNovaGetRandom` service.
- **Display and input** (`userland/netsurf/nsfb_novaos.c`): a libnsfb
  surface whose framebuffer *is* the window's client bitmap, so NetSurf's
  plotters draw straight into the desktop window; keyboard (with key-up
  events, now delivered to program windows) and mouse come from the Win32
  message queue.  The browser window can be resized from any edge,
  maximized, snapped to half the screen and restored: `WM_SIZE` becomes a
  libnsfb resize event, the toolbar, scroll bars and status bar move, and
  the page is laid out again for the new width.
- **Text** (`userland/netsurf/font_nova.c`): anti-aliased TrueType text
  with sub-pixel positioning, rendered at run time by
  [stb_truetype](https://github.com/nothings/stb) from Inter (sans-serif)
  and DejaVu Sans Mono (monospace) in `C:\Windows\Fonts`; italic is
  synthesized.  JPEG decoding uses stb_image (`jpeg_stb.c`).
- **The C runtime grew a POSIX layer** for it (`userland/msvcrt/posix.c`,
  `iconv.c`, headers in `userland/include/posix`): file descriptors with
  `dup`/`fdopen`/`pread`/`pwrite`, `stat`, `opendir`/`scandir`,
  `getopt_long`, `gettimeofday`, `iconv` (UTF-8/16/32, Latin-1,
  Windows-1252), `asprintf`; `crt0` now runs static constructors.
- **JavaScript** (Duktape 2.x, NetSurf's engine, with its generated DOM
  bindings): page scripts, external scripts, `setTimeout`/`setInterval`,
  events (`addEventListener`, `onclick`), JSON, `Date`, and navigation from
  script run; a failing script does not stop the page.  It is on by
  default; `enable_javascript:0` in `C:\Programs\NetSurf\res\Choices`
  turns it off.  Like NetSurf 3.11 on every platform, changes a script makes
  to the page *after* it has been laid out are not redrawn yet.
- Not yet: SVG and IPv6.  *(IPv6 came with "IPv6, HTTP/2 and virtio-net
  (Phase 18.8)".)*

## Desktop UX refresh
- **Start menu** (`wm/desktop.c`): live search as you type (Win key, then
  type) over the built-in apps, the programs in `C:\Programs`, Settings
  pages ("display", "wallpaper", "wifi"...) and every file and folder on C:.
  Up/Down pick a result, Enter opens it, Esc clears the query and then closes
  the menu.  The home view shows pinned apps, the installed programs (click
  to run in a Terminal), recently used apps, files and folders, and a footer
  with Files, Settings and a power menu (Restart, Shut down).
- **Window management** (`wm/wm.c`): drag a title bar to the left or right
  edge to snap to half the screen, or to the top to maximize (a translucent
  preview shows where it will land); dragging a tiled window restores its
  size.  Resize windows from any edge or corner.  Keyboard: **Alt+Tab**
  (Shift+Alt+Tab backwards) with a switcher, **Win+Left/Right/Up/Down**
  (snap, maximize, restore/minimize), **Win+D** (show desktop and back),
  **Win+E** (File Explorer), **Win+S** (search).
- **Right-click menus** on the desktop (Terminal, File Explorer, show
  desktop, next wallpaper, Personalize, Display settings), desktop icons
  (Open, Open in Terminal), title bars (maximize/restore, snap, minimize,
  close) and dock items (open or new window, close).
- **Dock and tray**: the Start menu, dock and tray are neutral dark
  frosted glass (`GdiBackdrop` blurs whatever is behind them, then tints
  it), so they sit well on any wallpaper.  The dock holds only launchers
  and windows, on a fixed grid (40 px targets, 28 px app tiles, 22 px
  glyphs); every window without a pinned app gets its own button with a
  running indicator.  The tray, a separate block at the right, shows the
  network status (tooltip: address; click: Settings > Network) and the
  clock (tooltip: full date; click: Calendar).
- **Themes**: three wallpapers (Sunset, Ocean, Twilight), chosen in the new
  **Settings > Personalization** page.
- **One icon style**: apps are rounded-square tiles with a white line glyph;
  folders (special ones show their purpose: Documents, Downloads, Pictures,
  Projects), documents and This PC are flat two-tone shapes; sidebars,
  toolbars and the tray use single-colour line glyphs; every line is 8% of
  the icon size thick.  The Start button is NovaOS's star.
- **Windows**: the title bar is a shade darker than toolbars, with a
  hairline under it, and every window has a faint light edge.
- **File Explorer**: a command bar with back/up, a clickable breadcrumb
  path (This PC > Local Disk (C:) > Documents; long paths collapse from the
  left) and "+ Folder" / "+ File" buttons; the sidebar has line glyphs per
  place (now including Downloads) and is divided from the list.
- **Desktop**: This PC, Documents, Downloads, Pictures and Projects open
  File Explorer there; NetSurf has an icon.  The selection is a soft
  translucent fill, and labels have a blurred drop shadow so they stay
  readable on bright wallpaper.
- **Windows icons (.ico)** (`gdi/icon.c`, `gdi/png.c`): the kernel reads
  .ico and .cur files, whose images may be BMP-style DIBs (1, 4, 8, 16, 24
  or 32 bits per pixel with the AND mask) or PNG (as 256x256 Vista-style
  icons are), and the icons in a program's resources (RT_GROUP_ICON /
  RT_ICON in any .exe or .dll).  Drawing picks the image that best fits the
  size at device resolution and scales it with alpha blending.  PNG is
  decoded by a small built-in inflate + PNG decoder (every colour type, bit
  depth and interlacing; it matches libpng on the PngSuite images).
  A program shows its own icon (a `NAME.ico` next to `NAME.exe` wins over
  the one in its resources) in the Start menu, search, the title bar, the
  dock, Alt+Tab and File Explorer; Explorer also shows .ico files as
  themselves and PNG thumbnails, with file types ("Icon", "Application",
  "PNG image").  Programs get icons at build time from `programs/NAME.rc`
  (compiled by `llvm-rc`): `winhello.exe` has one drawn by
  `tools/make_icons.py`, and `netsurf.exe` carries NetSurf's own.
- **Photos** opens .ico, .cur and .png files: the picture, and for icons
  every image in the file (size and colour depth) in a strip, the selected
  one enlarged pixel for pixel; Left/Right step through the folder, and on
  its own it shows a gallery of `C:\Pictures` (which now holds a sample
  icon and picture).  .exe files opened from Explorer run.
- Not yet: `LoadIcon`/`DrawIcon` and `WM_SETICON` for programs (a window
  shows its program's first icon), and animated cursors.  *(Phase 12's
  user32 has icons from `.ico` files and PE resources and `WM_SETICON`;
  animated cursors came with "Program pointers and animated cursors"
  below.)*

## Phase 10 — Standard DLLs, registry, COM and persistent storage
- **Unmodified Windows programs run**: stock release builds of ripgrep and
  fd (Rust, MSVC), jq (C, MinGW) and fzf (Go) work from the Terminal:
  searching, walking folders, filtering.  Copy an `.exe` onto the data disk
  (see [Where your files are kept](building.md#where-your-files-are-kept))
  and type its name.
- **The standard DLLs**, all built from source in `userland/`:
  - `ucrtbase.dll` and the `api-ms-win-crt-*` API sets: the Universal C
    Runtime, from the same sources as `msvcrt.dll` (with musl's libm), each
    with its own `printf` rounding; `vcruntime140.dll` has the MSVC **C++
    exception** machinery (`__CxxFrameHandler3`, `_CxxThrowException`,
    RTTI) as well as `memcpy` and friends.
  - `kernel32.dll` grew I/O completion ports, overlapped and alertable I/O,
    file mappings, waitable timers, `WaitOnAddress`, `CreateProcess` and
    `GetExitCodeProcess`, `FormatMessage`, NLS (`CompareString`,
    `LCMapString`, code pages), resources, psapi, and the registry.
  - `advapi32.dll` (tokens, SIDs, security descriptors, CryptoAPI hashes and
    random numbers, the event log, services), `bcrypt.dll` (SHA-1/2, HMAC,
    RNG), `shell32.dll` (known folders, `CommandLineToArgvW`,
    `ShellExecute`), `shlwapi.dll` (paths, strings, URLs, the `SH*`
    registry helpers), `psapi.dll`, `version.dll`, `winmm.dll`,
    `comctl32.dll`, `comdlg32.dll`, `userenv.dll`; `ws2_32.dll` gained the
    Winsock 2 extensions (`WSASend`/`WSARecv`, overlapped operations through
    completion ports, `WSAEnumProtocols`, `GetAddrInfoW`...); `user32` and
    `gdi32` cover about 400 and 120 functions (DIB sections, fonts, text).
  - Every program also sees `KUSER_SHARED_DATA` at 0x7FFE0000 (the Go
    runtime reads its clock there).
- **The registry** (`kernel/um/um_registry.c`, `userland/kernel32/registry.c`):
  real keys and values behind the `NtCreateKey`/`NtQueryValueKey` family,
  the whole `Reg*` API (advapi32 forwards to kernel32) with the predefined
  roots, and the usual contents (`CurrentVersion`, `CentralProcessor`,
  environment, shell folders, time zone...).  It is saved to
  `C:\Windows\System32\config\REGISTRY.DAT`.  `reg query|add|delete|export`
  works as on Windows.
- **COM** (`userland/ole32`, `userland/oleaut32`): `CoInitializeEx`,
  `CoCreateInstance` through registered class objects or
  `HKCR\CLSID\{...}\InprocServer32` DLLs (`DllGetClassObject`,
  `DllCanUnloadNow`), ProgIDs, `CoTaskMem*`/`IMalloc`, GUID strings,
  `CreateStreamOnHGlobal`; OLE Automation with BSTRs, VARIANTs and
  `VariantChangeType`, SAFEARRAYs, dates and error info.  `testdll.dll` is
  a sample in-process server, `regsvr32` registers it, and the SDK now has
  `objbase.h`/`oleauto.h`.
- **Persistent storage** (`kernel/drivers/ahci.c`, `kernel/fs/fat.c`,
  `kernel/fs/persist.c`): an AHCI SATA driver, FAT16/FAT32 with long file
  names (read, write, format), and drive C: saved to disk: changes are
  written a second after they happen and restored at boot.  NovaOS uses a
  volume labelled `NOVADATA`, formats an empty disk, or falls back to the
  boot disk.  Settings > Storage and the Terminal's `vol`/`sync` show and
  control it.
- Tests: `apitest` 46/46, `comtest` 49/49, `cppeh` 17/17 and
  `disktest write` / `verify` across a restart (150 files, passing on FAT32
  and FAT16, with `fsck.fat` finding the volumes clean).
- Not yet: dialog boxes, menus and child-window
  controls, file-open dialogs (they report "cancelled"), the MSVC FH4 C++
  exception tables, type libraries, `RegNotifyChangeKeyValue` events, audio,
  and a clipboard shared between programs.  *(Dialogs, menus and controls
  came in Phase 12, the shared clipboard after Phase 13, audio with HD
  Audio, and type libraries and FH4 in "COM type libraries and FH4 C++
  exceptions" below; the rest is still open.)*

## Phase 11 — Multiprocessor (SMP)

- **Every CPU core runs threads** (`kernel/ke/smp.c`): the kernel finds the
  processors in the ACPI MADT, starts each one with INIT and STARTUP IPIs
  through a real-mode trampoline (`arch/x86_64/ap_trampoline.asm`), and gives
  it its own KPCR (reached through GS), GDT, TSS and exception stacks, LAPIC
  timer, and idle thread.  Up to 16 CPUs.
- **Programs run in parallel**, and much of the kernel runs on every core at
  once too (below).
- **Per-core ready queues** (`ke/scheduler.c`): each core has its own run
  queue, lock and timed-sleep list, so cores schedule without touching each
  other.  A thread stays on the core it last ran on (warm caches); a new
  thread starts on the core that created it, and a core with nothing to run
  steals a ready thread from a busy one (every tick while idle, or at once
  when it is woken).  Making a thread ready wakes its own core if that one is
  idle, else any idle core, so the work spreads out.
- TLB shootdowns (IPIs) keep the other cores' page-table caches right when a
  program frees or re-protects memory; halted cores are woken by IPI when a
  thread becomes ready.  The clock follows the TSC, so it keeps time whatever
  the cores are doing.
- Programs see the core count: `GetSystemInfo`, the PEB,
  `KUSER_SHARED_DATA` and `NUMBER_OF_PROCESSORS`.  `cpus.exe` runs the same
  work on 1 thread and then on one thread per core: with `-smp 4` it reports
  a speedup of up to about 3.7× (less when the host's own cores are busy).
- **Finer-grained kernel locking**: the big kernel lock now belongs to
  threads (it nests, and the scheduler drops it when a holder is switched
  out), and these run without it, on all cores at once, under locks of
  their own:
  - the scheduler (each core's run-queue lock, held across its switches),
    the timer and IPIs;
  - memory: the physical page allocator and the kernel heap (spinlocks), a
    program's address space (its process lock), with TLB shootdowns before
    pages are freed;
  - waits, events, mutexes, semaphores and handle closing (the handle table
    under the process lock, object state under one spinlock, destructors
    taking the big lock themselves);
  - sockets (`net_lock` around the network stack), and program windows:
    message queues, `PostMessage`, `InvalidateRect` and timers have a
    spinlock of their own, and the desktop draws without the big lock under
    `DesktopLock` (the built-in apps' painters take it back);
  - the list, and what each service relies on, is in `um_syscall.c`
    (`um_lock_free_init`); files, the registry, process creation, the
    console and the loader still use the big lock.
  - the program loader holds `DesktopLock` only to look a module's file
    up: the file is then pinned (`RamfsPin`: its contents stay put, and
    writes to it fail, as Windows refuses writes to a mapped image) and
    copied, relocated, bound and committed without it, and without the
    big lock (`bkl_drop`; the work touches only the heap, the page
    allocator and the new image's address space); `LoadLibrary`
    serializes on a per-process loader lock instead.  Programs started from the Terminal,
    Explorer, `start` or the desktop are loaded on a worker thread
    (`UmSpawnStart`), so starting `node.exe` (90 MB) or loading CoreCLR
    no longer freezes the desktop for seconds.
- Waits are woken, not polled: `SetEvent`, `ReleaseSemaphore`, a thread or
  process ending, a message arriving or network data coming in wakes the
  waiting threads at once (wait queues, `ke/waitq.c`), and `select()` waits
  in the kernel.  Copies to and from program memory survive the memory
  being freed by another thread meanwhile (they fail with an access
  violation instead of crashing the kernel).
- `smpstress.exe` works all of this from many threads (critical sections,
  event ping-pong, semaphores, memory, handles, and freeing memory while the
  kernel copies into it) and checks the results.
- Boot QEMU with `-smp 4` (or any count) to use it.
- Also: window shadows skip the part their window covers (the desktop draws
  about twice as fast), and the network thread and ntdll's lock waits sleep
  instead of spinning.

## Phase 12 — Windows GUI programs: 7-Zip, unmodified

The goal of this phase is running real, unchanged Windows programs with
a graphical interface.  The test case is **7-Zip 26.03 (x64)** from
`third_party/7z2603-x64.exe`: the installer, the file manager (`7zFM.exe`)
and the GUI (`7zG.exe`) all run as shipped.  Nothing in 7-Zip was
changed; every fix is in NovaOS.

- **What 7-Zip does on NovaOS**: the installer shows its dialog, picks a
  folder with the "..." browser and installs with the progress bar; the
  file manager shows its toolbar, path box and file list (icons, sizes,
  dates, sortable columns), browses drives and folders, opens archives
  and lists their contents; Add to Archive opens `7zG`'s full options
  dialog and writes a real `.7z`; Extract goes through the Copy dialog
  and the Confirm File Replace prompt; Tools → Options opens with all six
  pages.  The file manager refreshes when files change on disk, and
  7-Zip's own icons show in the dock and title bars.
- **A real window system in user32** (`userland/user32/`): the window
  tree lives in the program, and only top-level windows have a desktop
  window (made when first shown); children are rectangles of their
  parent's bitmap, painted parent-first with invalidation cascading to
  siblings.  Per-thread message queues, sent messages across threads,
  timers, hit-testing, capture, double clicks, `WM_SETCURSOR`, mouse
  leave tracking, keyboard focus and `TranslateMessage`, A/W conversion
  of every message, hooks, properties, atoms, clipboard.  The standard
  controls (button, static, edit, list box, combo box, scroll bar), menus
  (bars, pop-ups, sub-menus, check marks, accelerators), dialogs from
  resource templates with tab and group navigation, `IsDialogMessage`,
  message boxes with icons, `DrawText` (wrapping, ellipsis, prefixes),
  `DrawEdge`, `DrawFrameControl`, icons from `.ico` and PE resources.
  `gdi32` draws TrueType text (Inter and DejaVu through stb_truetype)
  with clipping, viewport origins and raster operations.
- **The common controls** (`userland/comctl32/`), a real DLL that
  registers its classes when loaded: list view (details, list, small and
  large icons; owner data, callbacks, the Windows selection rules,
  keyboard, type-ahead, sorting, custom draw, label editing), header
  (resizing, ordering, sort arrows), toolbar (image lists and bitmaps,
  text, check groups, drop-downs), rebar, ComboBoxEx, tree view (lines
  and buttons, check boxes, expand/select notifications, label editing,
  sorting), tab control, property sheets (`PropertySheet` with the pages'
  dialogs, OK/Cancel/Apply/Help, wizards), progress bar, status bar,
  up-down, trackbar, tooltips, SysLink, image lists (ARGB, masks, icons),
  `DPA`/`DSA`, and real subclassing.  A program that uses a common control
  without linking `comctl32` gets it loaded on demand, as Windows does
  through the manifest.
- **The shell** (`shell32`): a folder picker (`SHBrowseForFolder` with
  the `BFFM_*` callback protocol), the system image lists with drawn icons
  for folders, drives and the common file types (`SHGetFileInfo`,
  `SHGetImageList`), `DragAcceptFiles`/`DragQueryFile` for dropped files.
- **Kernel sections**: named shared memory between processes
  (`NtCreateSection`, `NtMapViewOfSection`; `CreateFileMapping` and
  `OpenFileMapping` by name), which 7zFM and 7zG use to talk to each
  other.  File-backed mappings are sections too: filled from the file and
  written back on unmap, flush and last close, so a mapping of a file is
  shared between processes.  `shmtest.exe` checks both.
- **Drag and drop between programs**: OLE `DoDragDrop` with
  `IDropSource`/`IDropTarget` and `RegisterDragDrop` within a process, and
  across programs through the desktop, which tells the dragging program
  which window is under the pointer and carries the dropped file list to
  the other program; there it reaches the registered `IDropTarget` (as a
  `CF_HDROP` data object) or arrives as `WM_DROPFILES`.  `droptest.exe`
  has a source window and both kinds of target.  As on Windows the drop
  is synchronous: the dragging program's `DoDragDrop` returns only once
  the target has handled it, with the effect the target took, and no
  window keeps the mouse afterwards.  7-Zip's file manager depends on
  both: files dragged out of an archive are extracted to a temporary
  folder that 7-Zip deletes as soon as `DoDragDrop` returns, and its panel
  takes the mouse while it extracts.
- **Directory change notifications** (`FindFirstChangeNotification`): the
  kernel signals a program's event when a directory or its subtree
  changes.
- **Compatibility fixes found along the way**: x64 programs pass
  `HKEY_LOCAL_MACHINE` sign-extended (`0xFFFFFFFF80000002`);
  `SetWindowPlacement` must show a hidden window; `GetWindowPlacement`
  gives a child's position in its parent; `FindFirstFile` was dropping the
  file times; `GetDIBits` was dropping the alpha channel; static controls
  without word wrap still break lines.  `mpr.dll` (`WNet*`: no network),
  the `Lsa*` policy functions and `OpenFileMapping` were missing.
  Crashes now name the DLL and offset, a process's exit code is logged,
  `OutputDebugString` reaches the kernel log, and the Terminal's `trace
  NAME` prints a program's failing system calls.
- **Test programs**: `guitest.exe` (menus, accelerators, a resource
  dialog with combos, radios, checks and a list box, message boxes, a
  property sheet with a form page and a tree view page), `droptest.exe`,
  `shmtest.exe`, `ftprobe.exe` (what `FindFirstFile` reports).  Real
  programs are tested from a second disk image holding 7-Zip, Git, CMake,
  Ninja, Neovim, Notepad++ and others, driven by a QEMU harness that types
  Terminal commands, clicks, drags and takes screenshots.
- Drags from 7-Zip's file manager onto other programs work: one or more
  files from an archive or from a folder, onto a `WM_DROPFILES` window or
  an OLE drop target (tested with both `droptest` targets; each reports
  the dropped files' sizes, so a file that has already gone shows as
  missing).  (Pipes, `cmd.exe` and the clipboard: see below.)

## The App Store

The dock's **App Store** (`kernel/apps/store.c`, also `start store` in the
Terminal) is a catalog of free and open-source Windows programs: 7-Zip,
VLC, Firefox, Thunderbird, Notepad++, GIMP, Inkscape, Krita, Audacity,
HandBrake, OBS Studio, LibreOffice, SumatraPDF, KeePassXC, qBittorrent,
PuTTY, WinSCP, Git, Python, WinMerge and ShareX, plus a **Runtimes**
category (.NET Desktop Runtime, Visual C++ Redistributable, OpenJDK,
Mesa 3D), by category, with a note on how far each gets on NovaOS today.

- **64-bit packages where there are any.**  The store fetches each
  project's official 64-bit package: a portable `.zip` or `.7z`, an
  installer, a Windows Installer `.msi`, or, for Firefox and Thunderbird,
  the full installer, which is a 7-Zip self-extracting archive.  Apps
  whose only download is a 32-bit setup program (GIMP, qBittorrent,
  WinSCP, the Visual C++ Redistributable) now install through it too,
  since NovaOS runs 32-bit programs.
- **Get** downloads over HTTPS (following redirects, with the bytes
  received shown while it runs) to `C:\Downloads`, using the same network
  operations as the Terminal's `wget`.  Files may be up to 256 MB.
- **Install** depends on the package: archives are unpacked into
  `C:\Programs\<App>` by the installed 7-Zip's own `7z.exe`, unchanged
  (the store waits for it and reports its result, and asks for 7-Zip
  first if it is missing); `.msi` packages go to NovaOS's Windows
  Installer; installers, 64-bit or 32-bit, run.
  **Run** starts portable programs (PuTTY, SumatraPDF) from Downloads.
- **Open** starts the installed program from its own folder (console
  programs such as Python and Git in a Terminal); archives with a
  versioned top folder are found wherever the program landed.  The
  **Installed** view lists what is there; runtimes show "Installed".
- Tested in QEMU with stand-in packages (the sandbox this was built in
  cannot reach the publishers): 7-Zip installed from its x64 installer;
  a portable zip unpacked into `C:\Programs\Notepad++` and opened; a
  7-Zip self-extracting installer unpacked like Firefox's and opened.
  The apps themselves mostly need more of Windows than NovaOS has (the
  note on each row).
- Programs in a folder of their own under `C:\Programs` now start in
  that folder when opened from the desktop (they used to be looked up
  by name and not found).
- The Terminal gained `copy <source> <destination>`.
- A kernel bug the store shook out: `ksnprintf` looped forever when a
  `%s` argument had to be cut to fit the buffer, freezing the desktop on
  a long error message.

## Windows Installer (.msi packages)

NovaOS has its own Windows Installer: `msi.dll` (`userland/msi/`) and
`msiexec.exe` in `C:\Windows\System32`.  Double-clicking a `.msi` in
Explorer, `start package.msi` in the Terminal, and the App Store's
Install button all go through it, and programs can call
`MsiInstallProduct`, `MsiConfigureProduct`, `MsiQueryProductState`,
`MsiGetProductInfo` and `MsiEnumProducts`.

- **The package format**: the OLE compound file container (FAT, mini
  FAT, DIFAT, the encoded stream names), the string pool and the
  column-major table streams, and the cabinets inside (or beside) the
  package with MSZIP (deflate) and LZX decompression, files spanning
  data blocks and cabinets.  These readers use only the C library and
  are tested on the host against packages built with msitools.
- **The engine** runs `InstallExecuteSequence`: launch conditions,
  `AppSearch`/`RegLocator`, feature and component selection (levels,
  the Condition table, `ADDLOCAL`/`REMOVE`, component conditions),
  Directory resolution onto NovaOS's folders (`ProgramFilesFolder` is
  `C:\Programs`, `SystemFolder` is `C:\Windows\System32`, ...),
  `CreateFolders`, `InstallFiles`, the Registry table (all value
  types, `[Property]`, `[#File]` and `[$Component]` formatting),
  `FindRelatedProducts`/`RemoveExistingProducts` through the Upgrade
  table, and product registration under the Uninstall key with a
  cached copy of the package in `C:\Windows\Installer`.  Custom actions
  that set properties or directories (types 51 and 35) run; ones that
  execute code are logged and skipped.  `msiexec /x` reverses it all,
  on the folder chosen at install time.
- **msiexec** takes `/i`, `/x` (a package or a `{ProductCode}`), `/qn`,
  `/qb`, `/passive`, `/l*v FILE` and `PROPERTY=value` overrides, and
  shows the familiar progress window with Cancel (full UI adds the
  completion message box).  Logs also go to the kernel log (`dmesg`).
- Custom actions, the packages' own dialogs, shortcuts, services and LZX
  on real packages came later; see "Windows Installer depth" below.

## Phase 13 — 32-bit Windows programs (WoW64)

NovaOS runs 32-bit (x86, PE32) Windows programs next to 64-bit ones, the
way 64-bit Windows does: the CPU runs them in compatibility mode under
the 64-bit kernel, and they get a 32-bit copy of the whole userland in
`C:\Windows\SysWOW64`.

- **Kernel** (`kernel/um/`): a 32-bit user code segment (0x38) and a flat
  4 GiB data segment; FS based at each thread's 32-bit TEB (switched with
  the thread); a 32-bit address layout, everything below 2 GiB (PEB, TEBs,
  loader list, stacks, heap and DLLs); the PE32 loader (4-byte import
  thunks, HIGHLOW relocations); x86 PEB, TEB and process parameters; DLLs
  looked up in SysWOW64, and file system redirection (a 32-bit program's
  `C:\Windows\System32` is `SysWOW64`, unless it calls
  `Wow64DisableWow64FsRedirection`).  32-bit code enters the kernel
  through `int 0x2E` with a block of 64-bit arguments.  A crash at a bad
  address now also names the caller in the log.
- **The 32-bit ntdll** (`ntdll_wow.c`) turns every system call's
  arguments into the kernel's 64-bit forms: handles sign-extended,
  pointers zero-extended, `OBJECT_ATTRIBUTES`, `UNICODE_STRING`,
  `IO_STATUS_BLOCK`, `CONTEXT`, `EXCEPTION_RECORD`, memory and
  thread/process information, handle arrays and window messages
  rebuilt in their 64-bit layout and the results copied back.
- **x86 exceptions** (`exc_x86.h`): frames chained from `fs:[0]`,
  `RtlUnwind`, `RtlRaiseException`, and the `__try` handlers of MSVC and
  clang (`_except_handler3`, `_except_handler4_common`); vcruntime140
  has x86 C++ exceptions (`__CxxFrameHandler3`, `_CxxThrowException`)
  and RTTI with absolute addresses; msvcrt has x86 `setjmp`/`longjmp`.
- **Build**: `tools/build_userland.py` builds the userland twice, the
  second time with `--target=i686-pc-windows-msvc`.  Stdcall functions are
  exported undecorated (`GetLastError`, not `_GetLastError@0`) through a
  generated `.def`, which also caught every mismatched calling convention
  between our DLLs at link time.  The 64-bit division helpers x86 code
  calls are in `lib/x86rt.c`.  32-bit builds of the test programs are in
  `C:\Programs\x86`; `NOVA_NO_WOW64=1` leaves the 32-bit pass out.
- **Programs see WoW64**: `IsWow64Process` is TRUE, `GetSystemInfo`
  reports an x86 machine and `GetNativeSystemInfo` the AMD64 one,
  `GetSystemDirectory` is `SysWOW64`.
- **Tested**: every self-test (crttest, filetest, threads/SEH, DLL/TLS,
  posixtest, apitest, comtest, C++ exceptions, shared memory) passes as a
  32-bit program as well as a 64-bit one; 32-bit GUI programs (winhello,
  guitest); programs from the 32-bit MinGW toolchain (C, and C++ with
  exceptions); 7-Zip's own 32-bit self-extractors, console and GUI,
  unpacking an archive; and a real NSIS (Modern UI) installer going
  through its welcome, folder, progress and finish pages, installing
  files, its uninstaller and registry keys, and uninstalling again.
- Pointer-size assumptions fixed on the way: TEB offsets in kernel32 and
  ws2_32, PE data directories in `GetProcAddress` and resources,
  `Get/SetWindowLongPtr` and the `DWLP_*` offsets, rename information,
  SRW locks, `%p`/`%z`/`%I` in printf and scanf, `FILE` (32 bytes on x86).
- Also fixed on the way (for 64-bit programs too): `EndDialog` called
  from a message another thread sent now ends the modal loop (NSIS's
  finish page); `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` records
  the operation in `PendingFileRenameOperations` instead of acting at
  once (NSIS uninstallers copy themselves to Temp and schedule that copy
  for deletion; it used to vanish before it could run); `shfolder.dll`
  exists (`SHGetFolderPath`, which NSIS takes from it); msvcrt exports
  `_controlfp`, `_control87`, `__p___initenv` and friends.  The
  Terminal's `trace` now shows the file name of file system calls.
- Not yet: pending renames are not carried out at the next start.  *(Since
  done: Session Manager's `PendingFileRenameOperations` run at boot, see
  "Phase 17: kernel and API correctness".)*

## Shortcuts (.lnk) and overlapping controls

- **`IShellLink`** (`userland/shell32/shlink.c`): shell32's ShellLink
  class, `CLSID_ShellLink`, registered under `HKCR\CLSID` (a bare
  `shell32.dll`, so 32-bit and 64-bit programs each get their own), with
  `IShellLinkW`, `IShellLinkA` and `IPersistFile`.  `Save` writes the
  Windows `.lnk` format (MS-SHLLINK: header, LinkInfo with the target in
  ANSI and Unicode, Unicode strings for the description, working folder,
  arguments and icon); `Load` reads links made by Windows too.
- **The shell uses them**: opening a `.lnk` (Explorer, the desktop, the
  Start menu, `start` in the Terminal) runs its target with the
  shortcut's arguments in its working folder, or opens the folder or
  document it points to; a shortcut shows its target's icon and is
  called "Shortcut" in Explorer.  The Terminal's `start` opens any
  document, folder or shortcut the way the shell does.  The Start menu
  lists the shortcuts in `C:\AppData\Roaming\Start Menu\Programs`
  (where `$SMPROGRAMS` and `CSIDL_PROGRAMS` point) ahead of
  `C:\Programs`, keeping "Uninstall ..."
  entries out of the grid (search finds them), and the desktop shows
  what is in `C:\Desktop` after its own icons, noticing new files within
  half a second.
- **Overlapping controls paint as on Windows**: siblings paint from the
  top of the z-order down (the first control of a dialog first), so where
  controls without `WS_CLIPSIBLINGS` overlap, the lower one's pixels end
  on top.  NSIS's page header is a white static above its bold title,
  subtitle and icon; they were hidden under it and now show, and the
  welcome and finish pages cover the header as they should.
- Tested with a real NSIS installer that makes a desktop shortcut (with
  arguments) and two Start menu shortcuts: the desktop icon appears, a
  double-click starts the 32-bit program with its arguments in its
  folder, the Start menu lists it, and the uninstaller takes them away.
  comtest round-trips a shortcut through `IShellLinkW`, `IPersistFile`
  and `IShellLinkA` in both 64-bit and 32-bit builds.

## Pipes and cmd.exe

- **Pipes in the kernel** (`kernel/um/um_pipe.c`): named pipes
  (`NtCreateNamedPipeFile`, `\\.\pipe\NAME` opened with `CreateFile`) and
  anonymous ones (`CreatePipe` makes a named pipe with a private name, as
  Windows does).  Each direction is a ring buffer; byte and message pipes
  (a reader in message mode gets one message per read, the rest with
  `ERROR_MORE_DATA`), `ConnectNamedPipe`, `DisconnectNamedPipe`,
  `WaitNamedPipe`, `PeekNamedPipe`, `TransactNamedPipe`/`CallNamedPipe`,
  `GetNamedPipeInfo`, `Set/GetNamedPipeHandleState` (message read mode,
  `PIPE_NOWAIT`) and instance limits.  The ends are kernel objects, so
  closing the last handle to one is what the other side sees: a reader
  gets what is left and then `ERROR_BROKEN_PIPE` (end of file), a writer
  `ERROR_NO_DATA`.
- **Overlapped I/O that really waits**: on a handle opened with
  `FILE_FLAG_OVERLAPPED` a read, write or `ConnectNamedPipe` that cannot
  finish stays pending (`ERROR_IO_PENDING`); whoever changes the pipe later
  finishes it, writing the data and the status into the waiting program
  and setting its event.  Completion ports get their packet and
  `ReadFileEx`/`WriteFileEx` routines run at the next alertable wait
  (through a kernel32 helper thread), `CancelIo`/`CancelIoEx` end requests
  with `ERROR_OPERATION_ABORTED`, and `GetOverlappedResult(Ex)` waits for
  them.  A 32-bit program's I/O status block goes to the kernel as its own
  (32-bit layout) so it can be finished later.
- **Handles for child processes**: handles carry an inherit flag
  (`SECURITY_ATTRIBUTES.bInheritHandle`, `SetHandleInformation`,
  `DuplicateHandle(..., TRUE, ...)`), and `CreateProcess(..., TRUE, ...)`
  gives the child every inheritable handle at the same value; the
  standard handles come from `STARTUPINFO` or, without
  `STARTF_USESTDHANDLES`, from the parent's own (so a program's children
  write where it writes).  `DuplicateHandle` works into and out of another
  process.  `DETACHED_PROCESS`/`CREATE_NO_WINDOW` start without a console.
- **The environment** is one sorted table in kernel32 that `SetEnvironmentVariable`,
  `GetEnvironmentStrings(A/W)` and `CreateProcess` all see; a child gets
  its parent's environment (or `lpEnvironment`, ANSI or Unicode) instead
  of a fixed default.  `ComSpec` names cmd.exe.
- **`NUL`** is the null device (`>nul`, `CreateFile("nul")` in any folder).
- **C runtime**: `system()`/`_wsystem()` run `cmd.exe /c`, `_popen`/
  `_pclose`/`_wpopen` read or write a command through a pipe, `_pipe`
  makes one, and reading a pipe whose writer is gone is end of file.
- **cmd.exe** (`userland/programs/cmd.c`, in System32 and SysWOW64): the
  command interpreter, interactive or `/c`/`/k`.  Lines are expanded
  (`%VAR%`, `%VAR:~1,2%`, `%VAR:a=b%`, `%ERRORLEVEL%`, `%CD%`, `%DATE%`,
  `%RANDOM%`; `%0`-`%9`, `%*` and `%~dpnxfatz0` in batch files; `!VAR!`
  with delayed expansion), then parsed: `&`, `&&`, `||`, `|`, `( )`
  blocks, `^` escapes and redirections (`<`, `>`, `>>`, `2>`, `2>&1`,
  `>nul`).  IF (`==`, `/i`, `not`, `EQU`...`GEQ`, `errorlevel`, `exist`,
  `defined`) with ELSE; FOR, `/d`, `/r`, `/l` and `/f` over files,
  strings and command output (`tokens=`, `delims=`, `skip=`, `eol=`,
  `usebackq`).  Batch files: labels, GOTO, CALL (files and `:labels`),
  SHIFT, SETLOCAL/ENDLOCAL (with `enabledelayedexpansion`), `exit /b`,
  ECHO ON/OFF and `@`.  Internal commands: echo, set (`/a` arithmetic,
  `/p` input), cd, dir (`/b`, `/s`, `/a`), type, copy, del, md, rd `/s`,
  ren, move, pushd/popd, path, prompt, start, pause, title, ver and more.
  Programs run with the redirected handles; each side of a pipe that is
  not a program runs in a child cmd.exe, as on Windows.  Also new in
  System32: `find`, `findstr` (its regular expressions), `sort`, `more`
  and `timeout`.
- **The Terminal hands lines to cmd.exe** when they use pipes,
  redirections or `&&`/`||`, and when they name a `.bat`/`.cmd` file;
  `cmd` starts it interactively.  Ctrl+C now stops every program on the
  console, not just the first.
- Tests: `pipetest.exe` (62 checks, 64-bit and 32-bit): anonymous pipes
  and end of file, a 300 KB write through a 4 KB pipe, children reading
  and writing redirected pipes, inherited handles by value, the
  environment, `cmd /c`, a pipeline into a program, `_popen`, `system`,
  `_pipe`, NUL, named pipes in byte and message mode, overlapped connect
  and reads with events, cancelling, a completion port and a completion
  routine.  `cmdtest.bat` (29 checks) covers expansion, SET /A, IF, the
  FOR forms, CALL, delayed expansion, pipes into `find` and `sort`,
  redirections and error levels.
- Not yet: `CREATE_SUSPENDED` is ignored and `CREATE_NEW_CONSOLE` shares
  the console; a file handed to a child has its own position (cmd.exe
  opens redirection targets for appending so output lands in order).
  *(Since done: `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` windows and shared
  file positions came with "Phase 17: kernel and API correctness".)*

## The clipboard

- **One clipboard for everything** (`kernel/wm/clipboard.c`, reached by
  programs through `NtNovaClipboard`): each format is a copy of its
  bytes; `CF_TEXT`/`CF_OEMTEXT` and `CF_UNICODETEXT` are converted into
  each other on request (with `CF_LOCALE`); formats a program registers
  travel by name, since each program numbers them differently.
- **user32** (`OpenClipboard` ... `GetClipboardData`,
  `EnumClipboardFormats`, `GetClipboardSequenceNumber`,
  `GetPriorityClipboardFormat`) now uses it, so text, files (`CF_HDROP`)
  and private formats copied in one program paste in another.
  `CF_BITMAP` travels as a `CF_DIB` and comes back as a bitmap; a format
  set with a NULL handle is rendered by its owner (`WM_RENDERFORMAT`) when
  the clipboard is closed.
- **The OLE clipboard** (`userland/ole32/clipbrd.c`): `OleSetClipboard`
  copies a data object's formats onto it, `OleGetClipboard` gives a data
  object that reads it (`GetData`, `QueryGetData`, `EnumFormatEtc`),
  `OleIsCurrentClipboard`, `OleFlushClipboard`.
- **The built-in apps**: Notepad selects (Shift with the arrows, Home,
  End, PgUp/PgDn; mouse drags; double-click for a word; Ctrl+A) and has
  Ctrl+C/X/V with Copy and Paste buttons.  The Terminal selects with a
  mouse drag (double-click: a word); Ctrl+C copies while something is
  selected (else it still interrupts), Ctrl+Shift+C always copies, and
  Ctrl+V, Shift+Insert or a right click paste into the prompt or the
  running program; the wheel scrolls.  File Explorer copies, cuts and
  pastes files and folders (Ctrl+C/X/V, Copy and Paste buttons) as
  `CF_HDROP` with a "Preferred DropEffect", so programs see them too.
- Tests: `cliptest.exe` (23 checks, 64-bit and 32-bit, each reading back
  in a second process): Unicode text read as `CF_TEXT` and the other way,
  a registered format by name, `CF_HDROP` with `DragQueryFile`, a bitmap
  as a 3x2 DIB and back, the sequence number, and the OLE clipboard.

## Git

MinGit's `git.exe` (2.47) runs: `--version`, `init`, `add`, `commit`,
`log`, `status`, `diff`, output into pipes (`git log | find`) and a pager
(`core.pager=more`).  What it needed from NovaOS:

- **A recursive loader lock**: a DLL's initialisation that itself loads
  a library (git's C runtime start-up does) spun forever on ntdll's
  loader lock; the lock now belongs to a thread and nests, as Windows'
  does, and a module being initialised is not initialised twice.
- **`OpenProcess` for other programs** (`NtOpenProcess`): git waits for
  its children by process id (`waitpid`); the handle shares the process's
  exit object, which keeps the exit code after the process is gone.
- **The C runtime's standard descriptors**: `dup2` onto 0-2 now moves the
  standard handle and `stdout`/`stderr` with it, and closing the last
  descriptor of a handle closes it even for 0-2, so a pager sees the end
  of its input.  New: `_spawn*`/`_exec*`/`_cwait`, `_flushall`, `_umask`,
  `_wchmod`, `_mktemp`/`_wmktemp`; `_vscprintf`, `_scprintf` and
  `_wfreopen` are exported.
- **The rest of its imports**: NUMA queries (`GetNumaHighestNodeNumber`,
  `GetNumaNodeProcessorMask`...), volume enumeration, `CreateRemoteThread`
  (this process), `PeekConsoleInput`, `GetSystemTimeAdjustment`,
  `NtSetEaFile`/`NtQueryEaFile`, `QueryServiceStatusEx`,
  `SetEntriesInAcl`, and Winsock's `WSAEventSelect`/`WSAEnumNetworkEvents`
  (a helper thread watches the sockets) and `getnameinfo`.
- **The whole MinGit layout**: the test disk carries MinGit unzipped as
  it is on Windows (`C:\Apps\MinGit` with `cmd`, `etc`, `mingw64` and
  `usr`).  Run as `C:\Apps\MinGit\cmd\git.exe` with nothing on `PATH`,
  git finds its templates (a new repository gets its sample hooks), its
  system `gitconfig` and its own helper programs; `checkout -b`, `merge`,
  `gc` (`pack-objects` and `repack` as child processes), `count-objects`
  and `fsck` work too.  That took two fixes:
  - **File names up to 255 characters** on drive C: (they were cut at 47);
    `gc` renames its pack to a 59-character temporary name.
  - **Opening a process that has exited but is still held**: `OpenProcess`
    now finds it while any handle keeps it, as on Windows, so `waitpid`
    on a finished child gets its exit code.
- Not yet: MinGit ships no `less`, git's default pager, so give `log` and
  `config --list` `--no-pager` or `-c core.pager=more`.  *(Since done:
  NovaOS's own `less`, see [A pager for git](#a-pager-for-git-lessexe).)*

## The MSYS2 runtime: `sh.exe`, `clone`, `push`

MinGit's shell and Unix tools (`usr\bin`: `sh.exe` is bash, `ls`, `cat`,
`wc`...) are MSYS2 programs, built on `msys-2.0.dll`, a fork of the
Cygwin runtime.  They run unmodified: `sh -c "..."` with pipes,
`$(...)`, subshells, globbing and redirection, starting Windows programs
and MSYS ones.  git starts `git-upload-pack` and `git-receive-pack`
through `sh`, so `git clone`, `fetch` and `push` between repositories on
C: work.  What the runtime needed from NovaOS:

- **Native API breadth**: object directories and symbolic links
  (`\BaseNamedObjects\...` with `RootDirectory`-relative names), timer
  objects, `NtQueryEvent`/`NtQuerySemaphore`, `NtOpenThread`,
  `NtRead/WriteVirtualMemory` of another process,
  `NtAllocateVirtualMemoryEx` and `NtMapViewOfSectionEx` with address
  requirements, `NtQueryObject` names (`\Device\HarddiskVolume1\...`,
  `\Device\NamedPipe\...`), more `NtQueryInformationFile`,
  `NtQueryDirectoryFile` and volume classes, tokens, SIDs, ACLs and
  security descriptors, LSA policy and account queries, and the
  `RtlGetCurrentDirectory_U` code shape the runtime searches for its
  `FAST_CWD` pointer.
- **Guard pages and stack growth**: `PAGE_GUARD` pages raise
  `STATUS_GUARD_PAGE_VIOLATION` once; on a thread's stack (from the TEB)
  the next page down becomes the guard and `StackLimit` follows, and new
  stack pages are read/write even when the reservation says
  `PAGE_NOACCESS`.  The runtime moves the main stack to its own area.
- **APCs at start-up**: the runtime queues its signal thread as an APC
  from a DLL's initialisation; ntdll now runs queued APCs once the loader
  is done, and alertable waits run them.
- **fork**: `STARTUPINFO.lpReserved2` reaches the child (the process
  parameters' `RuntimeData`), inheritable handles are really inheritable
  (`OBJ_INHERIT` and `bInheritHandle` on events, mutexes, semaphores,
  sections, timers and directories), so the child finds its parent's
  shared memory and rebuilds itself at the same addresses.
- **Pipes by directory handle**: the runtime opens `\Device\NamedPipe\`
  and creates its pipes relative to that handle.
- **Windows' directory listings**: every directory but a drive's root
  lists `.` and `..` first, so opening an empty directory succeeds (git
  moves pushed objects out of an empty quarantine folder).
- **Winsock tells sockets from pipes**: `WSAEnumNetworkEvents` and
  `WSAEventSelect` fail with `WSAENOTSOCK` on other handles, which is how
  git's `poll` finds out a pipe was closed.
- Smaller pieces: C runtime `swprintf` in msvcrt's legacy form (git's
  `git-*.exe` launchers), `_findfirst*`/`_findnext*`, console input queries
  failing for files, `GetConsoleWindow`, new stub DLLs (`iphlpapi`,
  `netapi32`, `secur32`, `authz`, `dnsapi`, `pdh`) and more kernel32
  (`VirtualAlloc2`, `MapViewOfFile3`, `QueryDosDeviceW`, console buffer
  and input calls...).
- Not yet: hard links (drive C: behaves like FAT, so git renames), and
  interactive `sh` sessions have not been tried; `sh -c` and scripts
  are what is tested.  *(Since done: `sh --login -i` runs interactively since
  Phase 17.2; hard links are still to come.)*

## Language runtimes: Java, .NET, Node.js, Python

The official Windows x64 builds of four runtimes install and run
unmodified, from their own installers or archives:

| Runtime | Package | Tested |
|---|---|---|
| Java (Eclipse Temurin 21) | JRE `.msi`, JDK `.zip` | `java -version`, a stress program (threads, exceptions, stack overflow, files, lambdas), `javac` compiling a program that then runs |
| .NET 10 | runtime + host from NuGet, Roslyn | `dotnet --info`, `dotnet hello.dll`, `dotnet csc.dll` compiling a C# test that then passes |
| Node.js 24 | `.msi`, `.zip` | `node -v`, `-e`, `npm -v` (`npm.cmd` through cmd), output into a pipe, a test script (crypto hashes and random bytes, fs and fs.promises, JSON, regex, exceptions, timers, environment) |
| Python 3.14 | NuGet package (`python`) | `-c`, a test script (hashlib, JSON, regex, files, exceptions, threads, sleep, environment, subprocess) |

`msiexec /i temurin-jre.msi /qn` and `msiexec /i node.msi /qn` install
into `C:\Programs`, write their registry keys and add themselves to
`PATH` through the MSI `Environment` table (`=`/`+`/`-`/`!`/`*` name
flags, `[~]` for prepending or appending to the current value).  New
processes build their environment from the registry
(`HKLM\...\Session Manager\Environment`, then `HKCU\Environment`, user
`Path` after the system one, `REG_EXPAND_SZ` expanded), so a new `cmd`
finds `java` and `node`.  What it took:

- **Loader**: implicit TLS for DLLs loaded later (every thread's TLS
  array grows; HotSpot keeps `Thread::current()` there), the system UCRT
  and API sets ahead of copies shipped next to a program (as Windows
  10 does), dependencies looked up in the folder of the DLL importing
  them (Python's `DLLs\`), `LOAD_LIBRARY_AS_DATAFILE`/`AS_IMAGE_RESOURCE`,
  IL-only assemblies of the other architecture mapped as data, 64-character
  module names, images over 64 MB (`node.exe`), full-path
  `GetModuleHandleEx`, and 8192 memory regions per process.
- **Code generators**: dynamic function tables (`RtlAddFunctionTable`,
  `RtlInstallFunctionTableCallback`, growable tables) for JIT-compiled
  code, and a fuller `RtlVirtualUnwind`: it reports where each
  register was saved (`KNONVOLATILE_CONTEXT_POINTERS`, which the .NET
  GC uses to update object references held in registers), finishes
  epilogues, follows chained unwind info and returns only the requested
  kind of handler.  `RtlCaptureStackBackTrace` walks real frames.
- **Threads**: fibers (`CreateFiber`, `SwitchToFiber`, `ConvertThreadToFiber`;
  the MSVC runtime asks `IsThreadAFiber`), `GetCurrentProcessorNumber`,
  XState context calls, `GetThreadIOPendingFlag`, NUMA and
  processor-group queries.
- **Locales without ICU**: .NET falls back to NLS, so `GetLocaleInfoEx`
  answers every `LOCALE_*` field for `en-US`, `en` and the invariant
  locale; `FindNLSStringEx`, `FindStringOrdinal`, preferred-UI-language
  calls, number and currency formatting; `LINGUISTIC_IGNORECASE` is
  honoured.
- **COM and WinRT**: `CoGetContextToken`/`CoGetObjectContext` return a
  real context object, `RoInitialize`.
- **Console and files**: console input queries succeed only for console
  input handles (libuv and Python decide whether stdin is a console that
  way), `*.*` matches names without an extension (Python's
  `encodings` package search), `NtDeviceIoControlFile`, and pipes answer
  `FileAccessInformation`/`FileModeInformation` (libuv opens a piped
  stdout as a pipe stream).  `PATHEXT` in the registry lists `.COM;.EXE;.BAT;.CMD`.
- **Processors in the registry**: `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\N`
  for every CPU (`os.cpus()` in Node).
- **New DLLs**: `crypt32` (empty certificate stores), `dbghelp`,
  `rpcrt4` (UUIDs), `powrprof`, `winhttp`, `mswsock`, and more of
  `iphlpapi`, `ws2_32`, `advapi32` (key-less CryptoAPI), `ole32` and the
  UCRT (`_create_locale`, conio, `_wspawnve`...).
- **Windows Installer**: the string pool's encoding of strings of 64 KB
  or more (Node's licence text), which shifted every later string id.
- Not yet: nothing from this list.  (MSI custom actions run since
  "Windows Installer depth", and .NET's globalization goes through ICU
  since "ICU: .NET globalization and kernel32's locales".)

## Installing NovaOS on a disk

The ISO is also the installation disc.  Booted from it, NovaOS runs
live (nothing is kept after a restart) and opens **Install NovaOS**
(`kernel/apps/setup.c`, the engine in `kernel/fs/setup.c`); an icon on
the desktop brings it back.  On an installed system it is in the Start
menu and `start setup` in the Terminal opens it, to copy NovaOS to
another disk.

- **Welcome, choose a disk, confirm, install, finish.**  Setup lists the
  SATA, NVMe and USB disks with their size and what is on them, marks the one NovaOS
  started from and the one drive C: is kept on, and asks before erasing
  anything.  Disks under 256 MB are shown but cannot be picked.
- **What it writes**: a GPT (protective MBR, primary and backup headers
  and entry arrays with their CRCs) with two partitions: an EFI System
  Partition (FAT32 `NOVA_EFI`, 128 MiB) holding `\EFI\BOOT\BOOTX64.EFI`
  and `\EFI\NOVA\kernel.elf`, and a basic data partition (FAT32
  `NOVADATA`, the rest of the disk) where drive C: is saved.  The copied
  kernel is read back and compared.  UEFI firmware finds
  `\EFI\BOOT\BOOTX64.EFI` by itself, so no boot entry is written.
- **Where the files come from**: booted from the disc, the bootloader
  sees a CD-ROM node in its device path and hands the kernel the two
  boot files in memory (boot protocol v3).  On an installed system Setup
  reads them from the disk NovaOS started from.
- **Your session comes along**: from the disc, drive C: lives on a blank
  disk that NovaOS formatted at boot, often the very disk being
  installed on.  Setup stops saving there before erasing it, then
  writes everything on C: to the new data partition and keeps saving
  there.  A C: kept on another disk stays where it is.
- **Restart now** reboots into the installed system (remove the disc
  first, or pick the disk in the firmware's boot menu).
- Tested in QEMU/OVMF: installing from the ISO onto a blank 1 GB disk,
  booting that disk alone with a file made in the live session still in
  Documents, and installing from the installed system onto a second
  disk.  Both new FAT volumes pass `fsck.fat`, and the GPT's checksums
  verify.
- To try it:

```bash
truncate -s 1G disk.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=disk.img,format=raw -cdrom nova.iso
# after installing: the same command without -cdrom starts from disk.img
```

## OpenGL: Mesa as the system `opengl32.dll`

Programs that draw with OpenGL get OpenGL 4.5 rendered on the CPU by
Mesa's llvmpipe, the same approach Wine takes: an existing open-source
DLL ships as an OS component, and the programs are not changed.  The
App Store's **Mesa 3D** (Runtimes) installs the
[mesa-dist-win](https://github.com/pal1000/mesa-dist-win) 24.2.4 build:
7-Zip unpacks just `opengl32.dll`, `libgallium_wgl.dll` and
`libglapi.dll`, 64-bit into `C:\Windows\System32` and 32-bit into
`C:\Windows\SysWOW64`, so every program that imports `opengl32.dll`
finds them.

- **WGL in gdi32** (`userland/gdi32/wgl.c`): `ChoosePixelFormat`,
  `DescribePixelFormat`, `GetPixelFormat`, `SetPixelFormat` and
  `SwapBuffers` load `opengl32.dll` on first use and call its `wgl*`
  functions, as on Windows.  GDI remembers each window's pixel format
  (set once), and a per-thread guard answers Mesa's own calls back into
  these from what GDI knows.
- **Presenting frames**: Mesa shows each frame with `StretchDIBits`.  An
  unscaled 32-bit blit now copies rows directly, and user32's
  `NovaFlushDC` puts the change on screen, since GL programs draw
  through a DC they keep for the window's life instead of painting in
  `WM_PAINT`.
- **x86 `SLIST_HEADER`** is 8 bytes, as on Windows (it was the 16-byte
  x64 layout, which 32-bit Mesa's aligned stores faulted on).
- Also `RtlGetLastNtStatus`, `HeapWalk`/`HeapLock`/`HeapUnlock` and
  `EnumDisplaySettingsA`.
- Tested in QEMU with `tools/gltest/gltest.c` built with MinGW, 64-bit
  and 32-bit, after installing Mesa from the App Store: vendor, renderer
  and version strings, clearing, immediate-mode drawing, a GLSL shader,
  pixel read-back, then animated frames with `SwapBuffers` (14/14 checks,
  about 48 frames per second in a 320x240 window under QEMU without KVM).  Mesa
  also tries its Zink (Vulkan) and D3D12 drivers first and logs that
  `vulkan-1.dll` and `d3d12.dll` are missing; it then uses llvmpipe.
- Next: Direct3D on top of this (WineD3D to OpenGL, or DXVK on Mesa's
  lavapipe Vulkan).

## App coverage: Notepad++, bat, fd

- **Notepad++ 8.7.9** (64-bit portable) opens with its menus, toolbar and
  editor, and takes typing.  What it needed: the `.exe`'s TLS slot is now
  always 0 (MSVC's thread-safe statics assume it), `GetFileAttributesEx`
  leaves its output alone when the file is missing, and `RtlUnwindEx`
  runs a consolidating unwind's callback, so `catch` blocks in programs
  with the static MSVC C++ runtime run (a `throw` used to resume after
  the throw site).
- **bat** (`CompareObjectHandles`, a new `NtCompareObjects` call) and
  **fd** (`GetModuleHandle` of an API set name) now run; rg and jq ran
  already.
- **New DLLs**: `uxtheme` (no theme; real buffered paint), `dwmapi`
  (composition off), `imm32` (no IME), `msimg32`, `wintrust` (nothing is
  signed), `sensapi`, `wininet` (URL parsing; offline).  `kernel32` has
  `.ini` files (`GetPrivateProfileString` and friends), `gdi32` gradient
  fills, pattern brushes and coordinate conversion, `crypt32`
  `CryptStringToBinary`.
- **`tools/novarun.py`** boots the image in QEMU with programs copied onto
  a data disk, types Terminal commands and takes screenshots:
  `python3 tools/novarun.py --put 'DIR=C:\Apps\x' 'cd C:\Apps\x' 'x.exe' '!shot x.png'`.
- Not yet: ~~Notepad++'s status bar draws black and its toolbar is cut
  short~~ (fixed in Phase 17.6); ~~Neovim hangs on exit (console input handles cannot be waited
  on)~~ fixed in Phase 17.2;
  ffmpeg needs `avrt`, `ncrypt`, `d2d1`, `dwrite` and more (see
  [More compatibility](#more-compatibility-schannel-uniscribe-idn-crt-gaps)).

## ACPI power: shut down, restart, power button

- **`kernel/hal/acpi.c`** reads the FADT (PM1 event and control blocks,
  the reset register, hardware-reduced sleep registers) and finds the S5
  sleep type in the `\_S5` package of the DSDT or an SSDT, which is plain
  data, so no AML interpreter is needed.  It switches the chipset into ACPI
  mode through `SMI_CMD` and enables the fixed-feature power button.
  The MADT lookup in `smp.c` now goes through `AcpiFindTable`.
- **Shut down** writes `SLP_TYP | SLP_EN` to PM1a/PM1b control, falling
  back to the virtual machines' ports.  **Restart** uses the FADT reset
  register, then port `0xCF9`, then the 8042, then a triple fault.  Both
  save drive C: first and show a "Shutting down" / "Restarting" screen.
- **Power button**: the desktop polls `PWRBTN_STS` and shuts down, as
  Windows does by default (`system_powerdown` in the QEMU monitor).
- **Programs**: `NtShutdownSystem`, `ExitWindowsEx` (shut down, power off,
  restart; no log-off), `InitiateSystemShutdown[Ex]` (at once; there is no
  countdown to abort), and a `shutdown.exe` (`/s`, `/p`, `/r`).

## USB keyboards and mice (xHCI)

- **xHCI host controller driver** (`kernel/drivers/xhci.c`): takes the
  controller from the firmware, resets it, and runs one command ring and
  one event ring.  Like the disk and network drivers it is polled: the
  timer tick drains the event ring, so no interrupt routing is needed.
- **Enumeration**: each connected root port is reset, given a device slot
  and an address, and its device and configuration descriptors are read.
  The first HID boot-protocol keyboard or mouse interface is configured
  (Configure Endpoint, `SET_CONFIGURATION`, `SET_PROTOCOL(boot)`) and a
  transfer is kept queued on its interrupt endpoint.
- **HID** (`kernel/drivers/usbhid.c`): keyboard reports become set-1
  scancodes, the codes a PS/2 keyboard sends, so the window manager and
  `user32` see one kind of keyboard; held keys repeat (500 ms, then every
  30 ms) as PS/2 keyboards do on their own.  Mouse reports (buttons, motion,
  wheel) go to the same input queue as PS/2 mouse packets.
- **Hot-plug**: a small `usb` kernel thread enumerates devices plugged in
  after boot and releases the keys of a keyboard that is pulled out.
- `PciMapBar` maps BARs that lie above the 64 GiB physmap (OVMF puts 64-bit
  BARs at 512 GiB and up); the PS/2 driver now notices when there is no
  8042 controller instead of reading phantom bytes.
- Tested in QEMU with `-machine q35,i8042=off -device qemu-xhci -device
  usb-kbd -device usb-mouse`: Terminal commands typed and the pointer moved
  over USB alone, and a keyboard added and removed with `device_add` /
  `device_del` while running.
- Not yet: a keyboard's media keys and a mouse's extra buttons.  (Done
  since: hubs, absolute pointers and report protocol in Phase 18.1, USB
  mass storage in 18.2, and keyboard LEDs, several controllers and the
  older UHCI/OHCI/EHCI controllers under "Older USB controllers".)

## Direct3D: DXVK on Mesa's Vulkan

Programs that draw with Direct3D 8, 9, 10 or 11 get it rendered on the
CPU: [DXVK](https://github.com/doitsujin/dxvk) translates Direct3D to
Vulkan, and Mesa's lavapipe runs the Vulkan on the CPU (llvmpipe again
underneath).  As with OpenGL, these are existing open-source DLLs shipped
as OS components, and the programs are unchanged.  WineD3D (Direct3D to
OpenGL) was the other candidate; its source was out of reach from the
build machine, and DXVK is the faster, more complete path anyway.

- **App Store**: **Mesa 3D** now also installs `vulkan_lvp.dll` and its
  manifest (`lvp_icd.x86_64.json`, `lvp_icd.x86.json`) and registers the
  manifests under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, as a driver
  installer does.  The new **DXVK** (Runtimes) installs `d3d8`, `d3d9`,
  `d3d10core` and `d3d11` into `System32`/`SysWOW64`, and DXVK's `dxgi`
  as `dxgi_dxvk.dll`.  The Store gained `.tar.gz` downloads (7-Zip takes
  the `.gz` layer off first), `path>name` renames, and the kernel helper
  `um_registry_set_dword` for installers' registrations.
- **`vulkan-1.dll`, NovaOS's own** (`userland/vulkan-1/`): finds the
  driver from that registry key (or `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`),
  loads the first manifest's library that suits the process (64- or
  32-bit), negotiates the driver interface, and hands programs the
  driver's commands directly.  It exports the Khronos loader's 246 names;
  most are a jump through a pointer filled when the program creates its
  instance.  There are no layers and one driver at a time.  Mesa's Zink
  now finds it too, declines the CPU device, and OpenGL stays on llvmpipe.
- **`dxgi.dll`, NovaOS's own** (`userland/dxgi/`): a factory with no
  adapters, so programs that import DXGI start; with DXVK installed, it
  hands out DXVK's factory instead.  A Khronos `vulkan-1.dll` that a
  program carries beside its `.exe` loads `System32\dxgi.dll` and needs a
  factory before it reads its drivers; it keeps getting NovaOS's, since
  DXVK's would start Vulkan again and deadlock on DXVK's own lock.
- **`cfgmgr32.dll`** with empty device lists, for the loader and DXVK.
- **Frames on screen**: lavapipe presents with `StretchBlt` from a DIB
  section to the window's DC.  `BitBlt` and `StretchBlt` now copy rows
  for unscaled 32-bit copies and flush window DCs to the screen through
  `NovaFlushDC`, which skips a DC between `BeginPaint` and `EndPaint`
  (`EndPaint` presents).  The 64-bit D3D9 test went from 5 to 12 frames
  in three seconds at 320x240.
- **32-bit heap blocks are 16-byte aligned**, as on Windows: lavapipe's
  generated code reads them with `movdqa`.
- Also `AllocateLocallyUniqueId`, `EnumDisplayDevicesA`, the display
  configuration queries (`QueryDisplayConfig` and friends, answering "not
  supported"), `__C_specific_handler` from `kernel32` on x64, and the
  code bytes at a fault in JIT code in the serial log.
- Tested in QEMU with `tools/d3dtest/d3dtest.c` built with MinGW, 64-bit
  and 32-bit, after installing Mesa 3D and DXVK from the App Store: the
  D3D9 adapter (llvmpipe), a device, a fixed-function triangle read back
  through `GetRenderTargetData`, D3D11 at feature level 11_0 through a
  DXGI factory and adapter, a cleared render target read back through a
  staging texture, then animated frames with `Present` in a window
  (17/17 checks; D3D11 about 9 frames per second in a 320x240 window
  under QEMU without KVM).  OpenGL's `tools/gltest` still passes 14/14.

## Sound: Intel HD Audio, winmm and WASAPI

- **Driver** (`kernel/drivers/hda.c`): finds the HD Audio controller on
  PCI (class 04/03), resets it, talks to the codecs through the CORB/RIRB
  rings and routes every output pin with something attached (line out,
  speaker, headphones) through mixers and selectors to a DAC, unmuting
  the path at 0 dB.  One output stream plays a 64 KiB ring of 48 kHz
  16-bit stereo.  It polls: no interrupt routing or shared PCI code
  changed.  Tested with QEMU's `intel-hda` + `hda-output`/`hda-duplex`.
- **Mixer** (`kernel/drivers/audio.c`): up to 32 streams of 48 kHz s16
  stereo frames, each with its own queue and left/right volume.  A
  kernel thread wakes every tick, reads the hardware's position and mixes
  the running streams into the ring 80 ms ahead of it.  A stream reports
  frames written, mixed and actually played.
- **System calls** (`kernel/um/um_audio.c`, NovaOS-private like the
  socket ones): `NtNovaAudioOpen`, `NtNovaAudioWrite`, `NtNovaAudioCtl`.
  A stream is a handle object, so it stops when the program closes it or
  exits.
- **winmm**: `waveOut*` (open/write/pause/restart/reset/close, positions,
  volume, device caps; function, window, thread and event callbacks)
  converts any 8/16/24/32-bit PCM or float format, 1 to 8 channels, any
  rate, to the mixer's format.  `PlaySound`/`sndPlaySound` play files,
  `WAVE` resources, memory images and system sound aliases
  (`HKCU\AppEvents\Schemes`); NovaOS ships no `.wav` files, so the
  default sound is a synthesized two-note chime.  `waveIn`, MIDI and the
  mixer API still report no devices.
- **kernel32 `Beep`** plays its tone on the sound card; **`MessageBeep`**
  (and so every message box) plays the system sound.
- **mmdevapi** (WASAPI): `MMDeviceEnumerator` lists one render endpoint
  with its property store (friendly name, device format); `IAudioClient`
  (and `IAudioClient2`/`3`) in shared mode with event or polling
  clients, `IAudioRenderClient`, `IAudioClock`, `ISimpleAudioVolume`,
  `IAudioStreamVolume`, `IAudioSessionControl`.  The mix format is 48 kHz
  float stereo, and other PCM/float formats are converted.  **avrt**
  (`AvSetMmThreadCharacteristics`) raises the thread's priority.
- **Testing**: `soundtest` plays tones through each path;
  `tools/novarun.py --wav out.wav` gives QEMU a sound card that records to
  a file, and `tools/wavcheck.py` lists each tone's start, length, level
  and pitch.  Checked 64- and 32-bit: `waveOut` at 22.05 kHz mono and 48 kHz
  float, WASAPI, both at once (the mixer sums them at full level),
  `PlaySound` of an 8-bit 22.05 kHz file, the default sound and `Beep`;
  no discontinuities inside any tone.
- Not yet: recording, DirectSound (`dsound.dll`), XAudio2, MIDI,
  `IAudioEndpointVolume`, exclusive mode, and real programs (VLC,
  Audacity, SDL games) on it.

## ACPI sleep (S3)

- **Sleep**: Start > Sleep (right-click Start, or the power button in the
  Start menu), or `SetSuspendState`, `NtSetSystemPowerState` and
  `NtInitiatePowerAction`, which return once the machine is awake.
  Drive C: is saved first.  A keypress wakes it (`system_wakeup` in the
  QEMU monitor); the power button that wakes it doesn't also shut down.
- **CPUs** (`kernel/ke/sleep.c`): the other CPUs are stopped with an NMI
  and save their state; each CPU's control registers, GDT/TSS, IDT, MSRs,
  MTRRs and FPU state are kept, and the FACS waking vectors (real mode and
  32-bit) point at the CPU start-up trampoline, found in `\_S3`.  On wake
  the boot CPU restores itself and the PCI configuration of every function,
  restarts the other CPUs through the trampoline, and each CPU jumps back
  into what it was doing.
- **Devices** set up again: AHCI disks, e1000 network, PS/2 keyboard and
  mouse (with scancode translation forced on), the Bochs/QEMU display
  mode, xHCI USB (the controller restarts and the keyboards and mice are
  enumerated again) and HD Audio (codec paths and the output stream).  The
  wall clock moves on by what the CMOS clock measured.
- `sleeptest.exe` sleeps and then checks the clock, threads and files.
  Tested in QEMU (OVMF, q35) with 1, 2 and 4 CPUs, three sleeps in a row,
  and with a USB keyboard and mouse and HD Audio attached.
- Not yet: wake devices such as USB keyboards.  (Display modes on other
  adapters came later: see "Display adapters: QXL, virtio, VMware, Cirrus,
  and their modes after sleep"; USB wake came with "ACPI: SCI interrupt, lid,
  thermal zones, wake devices, _PRT (Phase 18.6)".)

## ACPI namespace (uACPI): batteries and AC power

- **AML interpreter**: uACPI 6.1 (`third_party/uacpi`, MIT) loads the DSDT
  and SSDTs.  `kernel/hal/aml.c` gives it memory, ports, PCI configuration
  space, time, locks and a work queue.  It runs on an `acpi` kernel thread
  that starts after boot and then polls the SCI every 100 ms (device
  interrupts stay off in NovaOS), running GPE methods and `Notify`
  handlers.
- **Batteries and AC adapters** (`PNP0C0A`, `ACPI0003`): `_BIX` or `_BIF`,
  `_BST` and `_PSR`, read every 5 s and on notifications, converted to
  mWh/mW.  `GetSystemPowerStatus` (kernel32), `CallNtPowerInformation`
  (`SystemBatteryState`) and `GetPwrCapabilities` report them through the
  new `NtPowerInformation`.  `battery.exe` prints them.
- **Power buttons**: once the namespace is loaded, the fixed button goes
  through uACPI and control-method buttons (`PNP0C0C`, `Notify 0x80`)
  work too.
- **Sleep**: `_PTS` and `_WAK` now run around S3, with only wake GPEs on
  while asleep.
- Tested in QEMU with extra SSDTs (`-acpitable`) describing a battery in
  mWh on battery power (75%, 3 h left) and one in mAh charging on AC
  (25%); sleep and the power button pass as before.

## Regression gate: boot CI on every pull request

- **GitHub Actions** (`.github/workflows/ci.yml`): every pull request and
  every push to main builds the kernel, bootloader, userland and
  `build/nova.img` on Ubuntu 24.04, boots it in QEMU (q35, OVMF, TCG) and
  runs the self-tests.  A failing test fails the "Build and boot-test"
  check; the step summary has a table of results and the serial log,
  screenshots and sound recording are kept as an artifact.
- **`tools/selftest.py`**: one boot, then each test typed into the
  Terminal.  A test passes on exit code 0, no `FAIL` line, no non-zero
  "failed" count and the output it expects; a kernel panic ends the run.
  The boot has an HD Audio card recorded to a WAV, which must hold the
  tones `soundtest` played, and the battery in `tests/acpi/battery.asl`
  (75%, 3 h left), which `battery` must report.
- **`guitest auto`** drives its own menus, edit and list boxes, the
  resource dialog, a message box and the property sheet, and reports.
- `tools/novarun.py`'s boot-and-type logic is now a `Nova` class that
  other tools import.

## Graphics tests in CI, ABI conformance, kernel backtraces

- **Graphics in CI**: a second CI job stages 7-Zip and the Mesa 3D and DXVK
  archives (`tools/ci/stage-graphics.sh`), installs both with the App
  Store, and runs `tools/gltest` (14) and `tools/d3dtest` (17), 64- and
  32-bit, with a screenshot of each while it draws.  The Terminal's new
  `store install NAME` presses a program's App Store button; the outcome
  goes to the serial log as `[STORE] NAME: Installed ...`.
- **`abitest`** checks NovaOS's binary interface against Windows 10 1903
  x64, each offset written out as Windows has it: the TEB, PEB, process
  parameters and loader lists, `KUSER_SHARED_DATA`, `CONTEXT` and
  `EXCEPTION_RECORD` (at compile time and at run time, through an
  exception handler that edits `Rip` and `Rax`, and `GetThreadContext` on a
  suspended thread), ntdll's stubs and all 464 system-call numbers, plus
  raw `syscall` instructions that bypass ntdll.  What it found and fixed:
  - 42 services had NovaOS numbers rather than 1903's (`NtQuerySystemTime`
    0x52 instead of 0x5A, `NtTerminateThread`, `NtResumeThread`, the
    registry, timer, directory and symbolic-link services, ...).  Every
    service Windows has is now at its 1903 number; NovaOS's own services
    moved to 0x200 and up.
  - ntdll's stubs are now Windows's bytes (`mov r10, rcx; mov eax, N; test
    byte [7FFE0308h], 1; jne; syscall; ret; int 2Eh; ret`), which
    sandboxes and hooking libraries parse.
  - The program now heads `InLoadOrderModuleList` and
    `InMemoryOrderModuleList` and is not in the initialization-order list.
  - `KUSER_SHARED_DATA.NtBuildNumber` said 19045; it is 18362, as the PEB
    and registry say.  `GetTickCount` reads the shared page's tick count,
    as on Windows, so both agree.
  - `RtlCaptureContext` fills in the segment registers.
  - `GetThreadContext` on a thread just suspended while running in user
    mode failed: the kernel waited a number of yields for it to stop, which
    can pass in microseconds; it now waits up to a second.
- **Symbolized kernel backtraces**: the kernel is linked twice; the first
  link's functions (`tools/mkksyms.py`) become a `.ksyms` table that the
  second link embeds after `.text`, so no function moves (the build checks).
  The kernel is built with frame pointers; a kernel page fault, exception,
  `KPANIC` or `KASSERT` prints `Backtrace:` and `#N address function+offset`
  frames on the serial log.  `crash kernel` (a new `NtNovaBugCheck`
  service, guarded by a magic argument) faults three calls deep to show
  it; CI checks the frames.

## Nightly app corpus

- **`tools/appcorpus.py`** downloads the official Windows x64 releases of
  ripgrep, fd, jq, 7-Zip, MinGit, Python (the NuGet package), Node.js and
  Notepad++ (portable), unpacks them into `C:\Apps` with a few sample files
  and a bare git repository, boots once and types each program's commands:
  a search, a `find`, a JSON filter, an archive made and tested, `git
  clone`, `log` and `status`, `python -c`, `node -e`.  Notepad++ opens a
  file and its screenshot is compared with `tests/reference/notepad++.png`
  (scaled down; at most 3% of pixels may differ).
- **`.github/workflows/nightly.yml`** runs it every night on main (and on
  pull requests that change the corpus) and posts the pass/fail table to
  the run's summary and as a comment on the "Nightly app corpus" issue.
- Not yet: Notepad++'s tab bar and status bar still draw black; the
  reference shows them so, and an improvement means updating it
  (`--update-reference`).  *(Since done: the bars draw since Phase 17.6, see
  "Phase 17: kernel and API correctness", and the reference was updated.)*

## More compatibility: Schannel, Uniscribe, IDN, CRT gaps

Driven by ffmpeg's imports (`tools/pe_imports.py`).  Before writing each
DLL we looked for an MIT, BSD or zlib licensed one to reuse; none existed
for these, so the new ones are written here, and Schannel reuses the Mbed
TLS already in the tree.

- **Schannel** (`userland/secur32/schannel.c`): `InitializeSecurityContext`,
  `EncryptMessage`, `DecryptMessage`, `QueryContextAttributes` (stream
  sizes, connection info, ALPN), `ApplyControlToken` (shutdown) and the
  `InitSecurityInterface` tables, on Mbed TLS with the Mozilla roots
  (`C:\Windows\System32\ca-bundle.der`).  It takes `SCHANNEL_CRED` and
  `SCH_CREDENTIALS`, SNI, ALPN, manual validation and
  `SCH_CRED_NO_SERVERNAME_CHECK`, and reports untrusted roots, expired
  certificates and name mismatches as Windows does.  Client side only.
- **New DLLs**: `usp10` (Uniscribe for left-to-right scripts:
  `ScriptItemize`, `ScriptShape`, `ScriptPlace`, `ScriptTextOut`,
  `ScriptBreak`, `ScriptString*`…), `normaliz` (`IdnToAscii`/`IdnToUnicode`,
  RFC 3492 Punycode), `ncrypt` (the provider opens; there are no stored
  keys), `avicap32` (no capture devices), `d2d1` (the matrix helpers;
  factories report `E_NOTIMPL`).
- **More of existing DLLs**: the CRT's `mbstowcs_s`/`wcstombs_s`, the
  `_nolock` functions, `freopen_s`, `tmpnam_s`, `_utime64`, `_wspawnvp`
  and the single-byte `_mbs*` set; `ws2_32` `getservbyname`/`getservbyport`,
  `gethostbyaddr` and `WSAPoll`; `winmm` `waveIn*` (no recording devices);
  `dnsapi` `DnsQuery_UTF8`; `iphlpapi` `GetIpForwardTable2` and friends;
  `advapi32` `RegLoadMUIStringW`; `shlwapi` `SHCreateStreamOnFileEx` and
  `StrRetTo*`; `ole32` `CreateBindCtx`, `ReadClassStm`/`WriteClassStm`,
  `OleSaveToStream`/`OleLoadFromStream`; `gdi32` DIB colour tables.
- **Loader**: a program's own TLS callbacks now run (process and thread
  attach and detach), not only those of DLLs; GLib checks for this.
- **Exceptions**: `RtlRaiseException` reports its caller's frame, so a
  handler that continues execution (the "set thread name" exception
  `0x406D1388`) resumes after the call instead of raising again forever.
- **`tools/novarun.py --net`** gives the guest a network card on QEMU's
  user network (the host is `10.0.2.2`).
- Tested with an FFmpeg nightly (BtbN's static x64 build) in QEMU, with
  the GDI and DirectWrite functions of the Firefox work in place: an x264
  encode, decoding it back, and streaming a WAV over HTTPS from the host
  with TLS 1.3 and with TLS 1.2; with `tls_verify` on (ffmpeg's default) a
  self-signed server is refused as an untrusted root.

## A pager for git: `less.exe`

`git log`, `diff` and `config --list` on the Terminal now page without
`--no-pager`.  MinGit has no `less`, git's default pager, so git stopped
with "unable to execute pager 'less'" (it does on Windows too).  NovaOS
now ships `less.exe` in `C:\Windows\System32` (and SysWOW64), on `PATH`,
where git finds it:

- It shows a screenful (the console's rows), then asks `-- More --` and
  takes single keys, through the per-key console input of Phase 17.2:
  Space (or `f`, Page Down) the next page, Enter (or `j`, Down) one more
  line, `d` half a page, `/text` and Enter skips to the next line
  containing the text, `q` (or Esc) quits, and git then stops quietly.
  The prompt is erased as the text moves on.  On a console that only
  hands over whole lines it asks for a line instead (Enter, a number,
  `/text` or `q`).
- Output that fits on one screen goes straight through, as with git's
  `LESS=FRX`, and so does everything when the output is not a console
  (`git log | find` is unchanged).  Color escapes pass through, files
  given as arguments (`less a.txt`) work, and options are accepted.
- Why not the real `less`: its Windows build draws through the console
  screen-buffer calls (`SetConsoleCursorPosition`, `FillConsoleOutput*`),
  which are still no-ops, so it would draw garbage; it can come once they
  drive the Terminal's screen.
- The nightly corpus's `git log` test no longer passes `--no-pager`.
- The test tools (`tools/selftest.py`, `appcorpus.py`) match a program's
  expected output without the kernel's `[UM]`/`[SCHED]` log lines, which
  share the serial port and could land mid-line (`[[UM] jq.exe ... 40,2]`).

## Phase 17: kernel and API correctness

- **Every thread has a real `ETHREAD`** (17.1): `PsGetCurrentThread` used
  to cast the running scheduler `Thread` to an `ETHREAD`, which is only
  true for threads made by `PsCreateSystemThread`.  Kernel threads from
  `sched_create_thread`, Windows programs' threads and the idle threads
  are bare `Thread`s, so writes through it (`PsInitialize` naming the boot
  thread TID 4, `PsTerminateSystemThread`) landed past the end of the
  structure.  `Thread.ethread` now leads to the thread's `ETHREAD`; a bare
  thread gets one (a real Thread object in the System process) the first
  time it asks, released when the scheduler frees it.  With interrupts
  off the lookup returns NULL rather than touch the heap.  The boot-time
  `[PSTEST]` self-test (`kernel/ps/ps_test.c`) checks it from plain
  kernel threads, including `NtCurrentThread()`,
  `PsLookupThreadByThreadId` and `PsTerminateSystemThread`.
- **Waitable console input and `WriteConsoleInputW`** (17.2): console
  input is a kernel queue of `INPUT_RECORD`s (keys, window size changes,
  what programs write into it) with a waitable object, so console input
  handles work with `WaitForSingleObject`, `RegisterWaitForSingleObject`
  and libuv.  `ReadConsoleInput`, `PeekConsoleInput`, `WriteConsoleInput`,
  `GetNumberOfConsoleInputEvents`, `FlushConsoleInputBuffer` and the
  console modes go through one NovaOS service, `NtNovaConsole`.  A program
  that turns off line input gets its keys as records, or as xterm
  sequences with `ENABLE_VIRTUAL_TERMINAL_INPUT`.
- **Full-screen programs in the Terminal**: while a program is on the
  alternate screen or reads raw input, its output goes to a libvterm 0.3.3
  (MIT) screen grid that the Terminal paints in colour, and libvterm's
  answers to terminal queries come back through console input.
- **What Neovim needed besides**: completion packets for overlapped pipe
  requests now go to the port the request started with, so closing a
  handle still delivers the cancellation libuv waits for (Neovim, and any
  `nvim -l` script, used to hang on exit); a `RegisterWaitForSingleObject`
  callback may unregister itself; `CreatePseudoConsole` and friends exist
  (and fail), which tells Neovim's `--embed` server to keep its RPC on the
  pipes and use `CONIN$`/`CONOUT$` for the terminal; the CRT reuses closed
  descriptors 0-2 first and moves the standard handles with them;
  `RtlRunOnceExecuteOnce`, `RtlUTF8ToUnicodeN`, `RtlUnicodeToUTF8N`, and
  UCRT `_o_` imports resolve to the plain functions.
- **What MSYS2 `sh` needed**: `\Device\Null` opens as the null device and
  counts as existing (Cygwin asks `NtOpenSymbolicLinkObject`); an empty
  name relative to a file handle reopens that file; a line feed on the
  screen grid also returns the carriage unless the program set
  `DISABLE_NEWLINE_AUTO_RETURN`.
- **Process creation flags** (17.3): `CREATE_SUSPENDED` starts the first
  thread suspended until `ResumeThread` (the same change as the Firefox
  work's).  `CREATE_NEW_CONSOLE` gives a console program a console of its
  own in a Terminal window titled with its path; the window closes when
  the program ends, and closing it ends the programs on that console.
  GUI programs ignore the flag, as on Windows, and `cmd`'s `start` uses it
  unless given `/B`.  `GetConsoleProcessList` now lists the processes on
  the caller's console.  A file's position belongs to the open file, not
  the handle: duplicates, inherited handles and handles passed as a
  child's standard output share it, so a parent and child writing to one
  log file follow each other instead of overwriting.  `proctest` covers
  all three in the core suite.
- **Security on objects** (17.4): tokens are kernel objects.  Each process
  has a primary token, inherited from the process that started it (the
  desktop user's otherwise: a standard user in Users, with Administrators
  only for denying since nothing is elevated), and a thread can
  impersonate an impersonation token.  `NtOpenProcessToken(Ex)`,
  `NtOpenThreadToken(Ex)`, `NtDuplicateToken`, `NtFilterToken`,
  `NtQueryInformationToken`, `NtImpersonateAnonymousToken`,
  `NtAccessCheck` and `NtQuery/SetSecurityObject` moved from ntdll into
  the kernel at their Windows 10 1903 numbers, and
  `NtSetInformationThread(ThreadImpersonationToken)` sets or ends
  impersonation.  `NtFilterToken` (advapi32's `CreateRestrictedToken`)
  makes SIDs deny-only, removes privileges and adds restricting SIDs, and
  can make the token write-restricted.  A named event, mutex, semaphore,
  timer, section, directory or symbolic link keeps the security descriptor
  it was created with (`SECURITY_ATTRIBUTES`), and opening it, or creating
  an existing name, checks the access asked for against it as the calling
  thread: the DACL in order, deny-only groups only in deny ACEs, the
  owner's implicit `READ_CONTROL | WRITE_DAC`, and for a restricted token a
  second pass with its restricting SIDs.  kernel32's `OpenEvent`,
  `OpenMutex` and `OpenSemaphore` pass the access asked for, not all.
  `GetKernelObjectSecurity`, `SetKernelObjectSecurity`, `GetSecurityInfo`
  and `SetSecurityInfo` read and change the descriptor; `CheckTokenMembership`,
  `ImpersonateSelf`, `RevertToSelf`, `SetThreadToken` and
  `ImpersonateLoggedOnUser` work on real tokens.  Files keep no
  descriptor yet: the file system takes that part in Phase 18 (18.5), on
  the same check (`um_access_check_sd`).  `sectest` and `acltest` (also
  32-bit) show a restricted token refused a protected event.
- **Registry change events and pending renames** (17.5):
  `RegNotifyChangeKeyValue` works, on the new `NtNotifyChangeKey`: a watch
  on a key (optionally with its subkeys) signals its event once when a
  value is set or deleted, a subkey is added, deleted or renamed, or the
  key itself is deleted, as its filter asks; without `async` the call
  waits for that.  `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` writes full
  `\??\` paths to Session Manager's `PendingFileRenameOperations` (with
  `!` for `MOVEFILE_REPLACE_EXISTING`), and the kernel now carries the
  list out at boot, after loading the registry and before any program
  runs, then deletes it.  A running program's files (its `.exe` and DLLs)
  are now held while it runs, as on Windows, so they cannot be deleted or
  replaced until it ends (renaming them still works).  The core suite
  runs an installer that has to replace a running program, restarts
  (`tools/novarun.py` and `tools/selftest.py` can restart NovaOS) and
  checks the replacement happened.  Hard links are still to come.
- **Small visible bugs** (17.6): File Explorer's This PC is now a list of
  the drives (C: and each mounted volume, D:, E:, ...) with their free
  space and size, and opening `C:\` or the desktop's This PC shows it; the
  up button and Backspace go from a drive's root back to This PC.  The
  Terminal's `dir` and cmd's `dir` name the drive they list and give that
  drive's own free space (cmd always asked C: before), and
  `GetDiskFreeSpaceEx` asks each volume (`FileFsFullSizeInformation`),
  C: included, whose free space is the free memory it lives in.
  Notepad++'s tab bar and status bar drew black: it double-buffers them by
  sending `WM_PRINT` into a memory DC, which `DefWindowProc` ignored.
  `WM_PRINT` now erases, sends `WM_PRINTCLIENT` and prints the children;
  the status bar, tab control and progress bar draw on `WM_PRINTCLIENT`;
  and the tab control lets its parent draw `TCS_OWNERDRAWFIXED` tabs
  (`WM_DRAWITEM`), sizes tabs from their text, icon and `TCM_SETPADDING`
  (`TCM_SETITEMSIZE`'s width only with `TCS_FIXEDWIDTH`) and takes
  `TCM_SETMINTABWIDTH`.  The nightly app corpus now boots with an empty
  NTFS drive D: and keeps screenshots of `dir C:\` and `dir D:\` (each
  with its own free space), This PC and Notepad++ (the last two compared
  with references in `tests/reference/`).
- **Off the big kernel lock** (17.7): files, the registry, the console and
  starting processes and threads now run beside each other on every CPU.
  The file system has a reader/writer lock: opening a file that is there,
  reading, writing, seeking and closing take it shared (each open file's
  contents under a lock of its own), and only changes to the tree take it
  alone.  The desktop thread's loop no longer holds it every tick, only
  around input, drawing and built-in windows' timers that may use files.
  The registry has a reader/writer lock of its own, a lock per key and
  value names on the stack.  A process's lock is a reader/writer lock too,
  and handles are opened, looked up and closed under it shared, each slot
  with its own lock; console output takes turns on the console's lock.
  Programs' memory allocation got arenas picked by thread (one heap lock
  was 18% of a 4-thread registry run) and stopped losing every freed small
  block (a tag bug kept them from being reused); kmalloc keeps a few
  objects per CPU; copies to and from programs move 8 bytes at a time and
  msvcrt's `memcmp` compares 8 at a time.  On one CPU in QEMU, files went
  from 1,800 to 18,000 operations a second and the registry from 5,000 to
  70,000; four CPUs do about 3x that (`smpstress scaling 3`; computing
  alone scales 3.7–4x on the same host).  smpstress also starts copies of
  itself from several threads at once, and the nightly run boots it on 4
  CPUs.  The Terminal's new `profile` command samples where the CPUs spend
  their time.
- Tested in QEMU: Neovim 0.10.4 and 0.11.4 open `t.txt`, take `ihello
  world<Esc>:wq` and exit with code 0 leaving the file written; MinGit's
  `sh --login -i` shows its coloured prompt and runs `ls`, pipes,
  `$(...)` and redirections to `/dev/null`; the core self-tests pass.

## Display adapters: QXL, virtio, VMware, Cirrus, and their modes after sleep

- **More adapters with the DISPI registers** (`kernel/hal/display.c`): the
  VBE driver now also drives QEMU's QXL (`-vga qxl`), virtio-vga
  (`-vga virtio`) and VMware SVGA II (`-vga vmware`, whose VGA core has
  them, with the framebuffer in BAR1) besides the standard VGA,
  bochs-display and VirtualBox's VBoxVGA.  The adapter table follows
  OVMF's QemuVideoDxe (BSD-2-Clause-Patent).  All of them get run-time
  resolutions and Settings > Display names the adapter.
- **Cirrus Logic GD5446** (`-vga cirrus`): 800x600 and 640x480 at 32 bpp,
  set with QemuVideoDxe's VGA and Cirrus register tables.  The bootloader
  now only picks 32-bit GOP modes, so Cirrus boots in 800x600 instead of
  its 24-bit 1024x768 mode, which drew garbled.
- **Modes after S3** (`DisplayResume`): the driver sets the current mode
  again on wake from what it knows, not from registers saved through I/O
  ports, so bochs-display (MMIO only) and Cirrus come back too; the page
  that was on screen stays on screen, and the desktop is redrawn in case
  video memory was lost.
- `tools/novarun.py --display NAME` boots on another adapter (`cirrus`,
  `vmware`, `qxl`, `virtio`, or a `-device` such as `bochs-display`).
- Tested in QEMU on all six adapters: switch to a non-boot mode with
  `disptest W H`, `sleeptest`, `system_wakeup`, and the desktop is back in
  that mode; also with page flipping (`-global VGA.vgamem_mb=64`).
- Not yet: real GPUs (Intel, AMD, NVIDIA) and virtio-gpu without VGA have
  no driver, so they stay on the UEFI framebuffer, and after sleep they
  show whatever the firmware's wake path sets up, which is often nothing.

## Program pointers and animated cursors (.ani)

Until now `SetCursor` only remembered its argument: the desktop always
drew its own arrow.  The pointer is now the program's, and animated
cursors play.  No MIT, BSD or zlib licensed .ani reader was found (Wine's
is LGPL), so the parser is written here; the format is a small RIFF file.

- **The kernel draws a program's pointer** (`kernel/wm/wm.c`,
  `kernel/gdi/gdi.c`): `NtNovaGuiCtl` op 19 hands it a shape (up to
  64 x 64 logical pixels, its hot spot, up to 64 frames and 256 steps,
  each step's time in jiffies), or asks for the arrow or no pointer.  It
  shows over the client area of that process's windows, and anywhere while
  one of them has the mouse captured; the desktop, title bars, borders and
  window drags keep the arrow.  The desktop's tick steps animated shapes
  (100 Hz against the .ani's 60 Hz jiffies) and redraws the pointer when
  the window under it changes.  Op 20 reports what the pointer shows, for
  tests.
- **user32**: `SetCursor` sends the cursor to the kernel when it changes
  (the system `IDC_*` cursors are the arrow; `NULL` hides the pointer),
  `ShowCursor` below zero hides it, and `GetCursorInfo` says whether it
  shows.
- **Animated cursors** (`userland/user32/res.c`): RIFF `ACON` files with
  `anih`, `rate`, `seq ` and the `fram` list of .cur/.ico frames, from
  `LoadCursorFromFile`, `LoadImage(LR_LOADFROMFILE)`,
  `CreateIconFromResourceEx` and `ANICURSOR`/`ANIICON` resources
  (`LoadCursor`, `LoadImage`).  `DrawIconEx` draws the frame of the step it
  is given, and `GetCursorFrameInfo` reports each step's frame and rate.
  `LoadCursorFromFile` loads .cur files too, and returns `NULL` for a
  missing file as Windows does (it used to return the arrow).
- **`anitest.exe`** (in the core self-tests) loads a spinner
  (`userland/programs/anitest.ani`, made by `tools/mkani.py`: 8 frames
  played in a custom order with two rates) from its resource, from memory
  and from a file, checks the steps, rates, hot spot and each step's
  drawing, then makes it the pointer over a window and checks the desktop
  shows it and steps through at least 6 of the 8 frames in 1.5 s, that
  `SetCursor(NULL)` hides it and the arrow comes back.  `anitest show N`
  keeps the window up for N seconds.  It runs as a 32-bit program too
  (`C:\Programs\x86\anitest`).
- Not yet: the system cursors themselves (I-beam, resize arrows, the
  busy and "working in background" animations) are all the arrow;
  `SetSystemCursor` does nothing; `CopyIcon` of an animated cursor keeps
  only its first frame; at 200 % the pointer is scaled up by nearest
  neighbour.

## COM type libraries and FH4 C++ exceptions

Two items left open since Phase 10.  We looked for MIT, BSD or zlib
licensed code first: Wine's typelib and FH4 code are LGPL, and nothing
permissive covers either, so both are written here from the file formats
(checked against `widl` output and real MSVC binaries).

- **Type libraries** (`userland/oleaut32/typelib.c`, `typeinfo.c`,
  `invoke.c`): `LoadTypeLib`/`LoadTypeLibEx` read MSFT-format libraries
  from `.tlb` files and from the `TYPELIB` resources of DLLs and EXEs
  (`file.dll\2` picks a resource), with `stdole2.tlb` built in (`IUnknown`,
  `IDispatch`, `IEnumVARIANT`).  `ITypeLib2`, `ITypeInfo2` and `ITypeComp`
  cover enums, records, coclasses, interfaces and dispinterfaces,
  including both views of a dual interface (`href -1`), default values,
  references into imported libraries and documentation strings (aliases
  and modules are read too, but no test library has them yet).
  `RegisterTypeLib`, `UnRegisterTypeLib` (and the `ForUser` forms), `QueryPathOfRegTypeLib` and `LoadRegTypeLib` keep
  `HKCR\TypeLib` and the interfaces' `ProxyStubClsid32` keys.
  `LHashValOfNameSys` gives a case-insensitive hash, not Windows' exact
  values (the lookups here compare names, so the hash is never needed).
- **Calling through type information**: `ITypeInfo::Invoke` (and so
  `DispInvoke`, `DispGetIDsOfNames` and `CreateStdDispatch`) converts
  DISPPARAMS to each method's own argument types, with named arguments,
  `[optional]` and `[defaultvalue]`, `[in, out]` by reference, `[retval]`
  and property puts, calls the vtable, and turns a failing HRESULT into
  `DISP_E_EXCEPTION` with the object's error info.  `DispCallFunc` calls
  any function or vtable slot (x64 register and stack arguments, x86
  stdcall and cdecl, floating-point and structure returns).
- **FH4** (`userland/vcruntime140/eh.c`, `vcruntime140_1.dll`):
  `__CxxFrameHandler4`, the compressed exception tables that MSVC has
  emitted for x64 since Visual Studio 2019, decoded into the same state
  machine as `__CxxFrameHandler3` (unwind maps, try blocks, catch
  continuations, separated code, `noexcept` functions).  The new
  `vcruntime140_1.dll` forwards to `vcruntime140.dll`, as Microsoft's does.
- **The sample COM server** (`testdll.dll`, `Nova.Calc`) now embeds its
  type library (`userland/testdll/idl/novacalc.idl`, compiled with `widl`
  by `make_tlb.sh`); its `IDispatch` is `DispInvoke` over that library and
  `DllRegisterServer` registers it.  DLLs can now carry an `.rc` file.
- **CRT**: the `<fenv.h>` functions (`fetestexcept`, `feclearexcept`,
  `fegetround`...) are exported from `msvcrt.dll` and `ucrtbase.dll`.
- Tests: the new `tlbtest` passes 110/110, 64- and 32-bit; `comtest`
  59/59 and `cppeh` 17/17, both architectures, all in the CI core suite
  now.  Python 3.14 with the kiwisolver 1.5.1 wheel, run against NovaOS's
  own `vcruntime140.dll` and `vcruntime140_1.dll` (Microsoft's copies
  removed from the Python folder), raises and catches kiwisolver's C++
  exceptions (`DuplicateConstraint`, `UnsatisfiableConstraint`,
  `UnknownConstraint`, `UnknownEditVariable`) through FH4 tables and gets
  the same results as on Linux.
- Not yet: NumPy still stops at the C99 complex functions (`cabs`,
  `cexp`...) the UCRT exports, and `AddDllDirectory` is a stub, so
  `os.add_dll_directory` paths are not searched.  `msvcp140.dll` (the C++
  standard library) is not provided.

## Complex text: HarfBuzz, FreeType and Uniscribe

Phase 19.1.  Arabic, Hebrew and the Indic scripts are shaped: letters join
into their contextual forms, ligate, reorder and run right to left.

- **`novatext.dll`** (`userland/novatext`) is the text core, built once and
  shared by Uniscribe, DirectWrite and Direct2D: **HarfBuzz 11.2.1**
  (`third_party/harfbuzz`, MIT) and **FreeType 2.13.3**
  (`third_party/freetype`, the FreeType License; TrueType, OpenType/CFF,
  Type 1, CID and `.fon` drivers, the smooth and mono rasterizers, the
  auto-hinter and the stroker).  It exports both libraries' C APIs (`hb_*`,
  `FT_*`).  No MIT Uniscribe or HarfBuzz-free shaper fitted, so this reuses
  the standard pair.  HarfBuzz is C++: `tools/build_userland.py` compiles it
  with clang against libc++'s headers (`libc++-dev`; nothing of libc++ is
  linked, and HarfBuzz needs no C++ runtime).  For that, the userland's
  `math.h` and `stdlib.h` gained the C++ overloads the Windows SDK's have,
  `locale.h` Windows' full `lconv`, and `include/cxx/functional` a slim
  `<functional>`.
- **`usp10.dll`** (`userland/usp10/usp10.c`) is rewritten on it.
  `ScriptItemize` splits text by script (HarfBuzz's Unicode data) and by
  bidirectional level (a compact UAX #9: strong letters, European and
  Arabic numbers, neutrals between them).  `ScriptShape` reads the DC's
  font through `GetFontData`, shapes the run with HarfBuzz at gdi32's
  pixel size and returns glyphs in visual order, logical clusters and
  visual attributes; it reports `USP_E_SCRIPT_NOT_IN_FONT` when the font
  lacks a complex script.  `ScriptPlace` hands out HarfBuzz's advances and
  mark offsets, `ScriptTextOut` draws marks at their offsets, and
  `ScriptCPtoX`, `ScriptXtoCP` and `ScriptGetLogicalWidths` handle
  right-to-left clusters.  New: `ScriptShapeOpenType`,
  `ScriptPlaceOpenType` (OpenType features), `ScriptItemizeOpenType`'s
  real script tags, `ScriptGetFontScriptTags`, `ScriptStringGetOrder`.
  The `ScriptString*` layer shapes each run with a fallback font when the
  DC's lacks its script (`SSA_FALLBACK`), lays the runs out in visual
  order and draws them all on the DC font's baseline.
- **GDI**: `ExtTextOut`, `TextOut` and `GetTextExtentPoint32` send text
  with complex-script characters through Uniscribe, as Windows' LPK does
  (`ETO_GLYPH_INDEX` and `ETO_IGNORELANGUAGE` skip it; `ETO_RTLREADING`
  makes the line right to left).  GDI gained the faces Noto Sans Arabic
  and Noto Sans Devanagari (`C:\Windows\Fonts`), also under Windows'
  names for those scripts (Traditional Arabic, Mangal, Nirmala UI...).
- **`usptest`** (in the CI core suite, 64- and 32-bit) checks itemizing,
  the Arabic contextual forms and lam-alef ligature, the Devanagari kssa
  conjunct with its reordered i sign and a half form, against the glyphs
  HarfBuzz gives on the build host, and that `ExtTextOut` draws mixed
  Latin, Arabic and Devanagari lines pixel for pixel as `ScriptStringOut`
  does.

## Direct2D

Phase 19.2.  `d2d1.dll` (`userland/d2d1`) is a software Direct2D.  No
MIT, zlib or BSD Direct2D exists (Wine's is LGPL), and the permissive
vector libraries (ThorVG, Blend2D, plutovg) could not be fetched here, so
it is written for NovaOS on a small core of its own:

- **Geometry** (`path.c`, `geometry.c`): figures of lines and cubic
  Béziers; rectangles, rounded rectangles, ellipses, arcs (SVG's endpoint
  form) and quadratic curves become them.  Path geometries and their
  sinks, transformed geometries and groups.  `GetBounds` is exact (curve
  extrema); `FillContainsPoint`, `ComputeLength`, `ComputePointAtLength`;
  `Tessellate`, `ComputeArea`, `CompareWithGeometry`,
  `CombineWithGeometry` (union, intersect, xor, exclude) and `Outline`
  cut the flattened shapes into horizontal slabs and join the result back
  into clean outlines.
- **Strokes** (`stroke.c`): each figure is flattened, cut into dashes
  (solid, dash, dot, dash-dot, dash-dot-dot and custom, with an offset),
  and outlined: miter (with its limit, clipped or bevelled), bevel and
  round joins; flat, square, round and triangle caps; separate start, end
  and dash caps.  `Widen`, `GetWidenedBounds` and `StrokeContainsPoint`
  use the same outline.
- **Rasterizer** (`raster.c`): exact-area anti-aliasing (each edge adds
  the area it covers to the cells it crosses, and a running sum gives
  every pixel's coverage), nonzero and even-odd fills, aliased mode, in
  bands.  Paint is source-over into premultiplied BGRA.
- **Brushes** (`brush.c`): solid; linear and radial gradients (with the
  gradient origin offset, clamp, wrap and mirror, sRGB or linear-light
  interpolation, from a 256-entry table); bitmap brushes with nearest or
  linear sampling and extend modes; bitmaps from memory or a WIC bitmap.
- **Render targets** (`target.c`): HWND targets (a DIB section copied to
  the window at `EndDraw`, `Resize`), DC targets (`BindDC`) and bitmap
  targets (`CreateCompatibleRenderTarget`), with DPI, transforms,
  axis-aligned clips, layers (opacity, geometric masks, opacity brushes),
  `FillOpacityMask`, meshes, drawing state blocks and
  `ID2D1GdiInteropRenderTarget`.
- **Text** (`text.c`): `DrawGlyphRun` fills the outlines from
  `IDWriteFontFace::GetGlyphRunOutline`; `DrawTextLayout` runs
  `IDWriteTextLayout::Draw` with Direct2D's text renderer (drawing effects
  that are brushes colour their ranges, underlines and strikethroughs are
  drawn); `DrawText` lays the text out with the shared DirectWrite
  factory first.
- **`tools/d2dtest`** (MinGW, so it uses MinGW's `d2d1.h` like any
  Windows program) checks geometry answers, draws a scene of fills,
  strokes, gradients, a dashed path of arcs, a bitmap and a rotated
  translucent rectangle, and compares it with the image
  `tools/d2dtest/reference.py` draws with Skia: under QEMU the mean
  difference is 0.24 of 255 per pixel.  It then checks layers, clips and
  an HWND render target in a window.  It runs in the CI graphics suite,
  64- and 32-bit.  Its text check draws "NovaOS" with `DrawText` and
  saves it as `d2dtext.bmp`.  DirectWrite's text formats and layouts (on
  HarfBuzz from `novatext.dll`) were written here and went in with the
  Firefox work, and with them the check passes, 64- and 32-bit.

## One file per item: parallel changes without merge conflicts

Up to seven pull requests were open at once, and nearly every one edited
the same lines of the same files: the DLL table and program sets in
`tools/build_userland.py`, the test lists in `tools/selftest.py` and
`tools/appcorpus.py`, and the lists in the README, `docs/HISTORY.md`,
`docs/ROADMAP.md` and `docs/building.md`.  Each merge left the others
conflicted, and twice conflict markers reached main.  Those lists are now
directories with one file per item, so changes add files instead of
editing shared lines.  [CONTRIBUTING.md](../CONTRIBUTING.md) is the guide.

- **DLLs**: `userland/NAME/dll.json` registers each DLL (dependencies,
  load addresses, extra source directories, entry point, implicit TLS,
  export ordinals); `tools/build_userland.py` finds them and links each
  after its dependencies.  A DLL that needs more (Mbed TLS for secur32,
  the msvcrt/ucrtbase double link) keeps that code in its own
  `userland/NAME/build.py`.  The existing DLLs keep their addresses; a
  new DLL leaves them out and gets a free 16 MiB slot, so two branches can
  no longer pick the same address (three open ones had all chosen
  0x7FFE50000000), and the build stops if two DLLs' images overlap.  The
  userland it builds is byte-for-byte the same as before (link timestamps
  aside).
- **Programs**: `userland/programs/NAME.json` replaces the 32-bit and
  System32 sets (`x86`, `system`, extra `libs`, `selftest`).
- **Tests**: one file per self-test in `tests/selftest/core/` and
  `graphics/`, one per program in `tests/appcorpus/`, run in file-name
  order; `tools/selftest.py --list` prints a suite.
- **Docs**: README's program table, "What is inside" list, licences,
  core-suite and self-test lists, the roadmap's "What comes next" items,
  building.md's self-test table and every HISTORY section are built by
  `tools/docgen.py` from files in `docs/readme/`, `docs/roadmap/`,
  `docs/selftests/` and `docs/history/` (and the tests' `DOC` strings).
  Pull requests add fragments and leave the generated regions alone; the
  Docs workflow (`.github/workflows/docs.yml`) rebuilds them on main after
  each merge.
- **CI**: a quick Checks job runs before the boot tests: no conflict
  markers or stray branch-name lines (`tools/ci/check-conflict-markers.py`),
  the manifests and test files load, and a pull request has not edited a
  generated region by hand.

## ffmpeg

Phase 19.3.  `tools/pe_imports.py` on a current Windows ffmpeg build
(BtbN's, statically linked, 168 MB) listed nine missing functions, now
added:
- gdi32: `ExtCreateRegion` (kept as a bounding box like every region),
  `GetGraphicsMode`, `Get`/`Set`/`ModifyWorldTransform` (kept per DC and
  reported back; drawing stays in device coordinates),
  `GetOutlineTextMetricsA`/`W` (from the font's head, hhea, OS/2, post and
  name tables) and `GetFontUnicodeRanges`.
- The CRT: `getenv_s` and `_wgetenv_s`.
- ws2_32: `WSASendMsg` (control data is not carried).
The tenth, `DWriteCreateFactory`, is the Firefox branch's DirectWrite.

Running it found three kernel bugs, all from ffmpeg's many threads:
- **Thread stacks**: new threads got a fixed 256 KB stack.  They now get at
  least the image's stack reserve, as on Windows (2 MB for ffmpeg, whose
  H.264 decoder threads overflowed the smaller stack into each other's).
- **Lost sleeps**: a thread woken early from a timed sleep stays on its
  CPU's sleep list until that CPU's next tick drops it.  A thread that
  exited in between was freed while still on the list, and every sleeper
  after it was lost: their `Sleep` never returned.  An exiting thread now
  leaves the list first, and a thread that moved to another CPU leaves the
  old CPU's list before sleeping on the new one.
- **Handles**: a process could hold 256 handles.  winpthreads makes events
  and semaphores for every mutex and condition variable, so ffmpeg ran out
  ("Cannot allocate memory", "Resource temporarily unavailable").  The
  limit is 4096.

`ffmpeg -i in.mp4 out.webm` (H.264 and AAC in, VP9 and Opus out) completes
under QEMU with its default threads, as do MPEG-4 encoding and 16 decoder
threads; the result plays on Linux.  The process dump on Ctrl+C now shows
each thread's scheduler state.  The nightly app corpus runs Gyan's ffmpeg
7.1.1 build (`tests/appcorpus/080-ffmpeg.py`): it makes an MP4 from its
test sources, converts it to WebM, and ffprobe must find VP9 and Opus.

## Firefox (Floorp)

Floorp 12.19, a Firefox build (the Firefox 157 engine), starts from the
Terminal, creates its profile and draws its full browser window.  The
browser is run as shipped; everything below is in NovaOS.

- **Imports**: the C runtime pieces Gecko uses (`_wsetlocale` and the
  rest), the delay-loaded DLLs it asks for, and cross-process
  `NtQueryInformationProcess`.
- **DirectWrite** (`userland/dwrite`): NovaOS's own `dwrite.dll`.  The
  factory, the system font collection (scanned from `%WINDIR%\Fonts`,
  with the common Windows family names mapped to the bundled fonts),
  font families, fonts, font faces (metrics, glyph indices, advances,
  kerning, outlines into a geometry sink, font tables), GDI interop and
  glyph run analysis (aliased and ClearType alpha textures).  Fonts are
  read with stb_truetype (public domain).  Text formats and text layouts
  (`layout.c`, written in the Phase 19 work for Direct2D's `DrawText`)
  break lines, handle bidirectional text, carry per-range font
  attributes, and answer metrics and hit tests.  They shape with
  HarfBuzz from `novatext.dll` when it is present, and with the font's
  plain glyphs and advances otherwise.
- **Kernel**: `NtQuerySection`, `MEM_RESET`/`MEM_RESET_UNDO`, a
  per-process handle table of 4096 (Gecko keeps far more than the old
  256 open), and `C:\AppData\Roaming`, `Local`, `LocalLow` and
  `C:\ProgramData` made at boot.
- **C runtime**: `_vsnwprintf` (the legacy option of
  `__stdio_common_vswprintf`) now fills a buffer exactly, without the
  terminator, when the output is exactly the buffer's size.  Gecko formats
  its 16-digit install hash that way; returning -1 made the profile
  service fail and Firefox show "Profile Missing".
- **user32**: window class names up to 256 characters (Gecko's remote
  window class contains the profile path).
- **Window station security**: the sandbox's alternate desktop reads and
  sets the window station's and desktop's security with `GetSecurityInfo`
  and `SetSecurityInfo` (`SE_WINDOW_OBJECT`).  Those are user32 pseudo
  handles, so they answer with the default descriptor instead of failing;
  the failure had left the broker without a desktop and crashed it.
- **Debugging aids**: the kernel prints each new process's command line;
  `tools/novarun.py` takes `!bg COMMAND` to leave a program running while
  it waits and takes screenshots, and `NOVARUN_GDB=1` starts QEMU with a
  gdb server so breakpoints can be set in a program's code.
- **Sandbox** (Chromium's, which Firefox uses for its child processes):
  - ntdll's system call exports have the Windows byte layout (see the
    ABI conformance work below), so the sandbox can copy and patch them
    to intercept calls in the child.
  - Token handles know whether they are primary or impersonation tokens
    and at which level; `SetThreadToken`, `OpenThreadToken`,
    `ImpersonateSelf` and `RevertToSelf` track a token per thread.
    `CreateWellKnownSid` covers every well-known SID type.
  - New system calls: `NtOpenProcessToken(Ex)`, `NtOpenThreadToken(Ex)`,
    `NtImpersonateAnonymousToken`, `NtQueryFullAttributesFile`,
    `NtSetInformationProcess`, and the `ProcessHandleCount` and
    `ProcessHandleTable` classes of `NtQueryInformationProcess`.
  - `CREATE_SUSPENDED` really suspends a new process, so the parent can
    patch the child before it runs.
  - ntdll exports the heap and string functions the sandbox resolves in
    the child (`RtlCreateHeap`, `NtSignalAndWaitForSingleObject`,
    `_strnicmp`, `wcslen`...), and `GetProcessHeaps` includes an empty
    csrss port heap the sandbox expects to find before it cuts a content
    process off from csrss.
  - A process can have 256 threads (was 64); Firefox's main process runs
    more than 64.
- **Overlapped I/O**: a pipe read or write that fails at once (a broken
  pipe when a child process exits) no longer sets its event or queues a
  completion packet or routine; Windows does none of these, and Firefox's
  IPC and Rust I/O free the `OVERLAPPED` after such a failure, so the late
  packet crashed the main process with a use-after-free.
- **GDI**: `CreateDIBSection` with a file-mapping handle puts the pixels in
  that mapping (Firefox's GPU process draws the browser into one shared
  with the main process).
- **Window handles across processes**: an `HWND` now names the same
  window in every process, as on Windows.  user32 builds each handle from
  a tag the kernel gives the process (unique among running processes), so
  handles never collide, and tells the kernel each desktop window's handle
  and client area.  `IsWindow`, `GetClientRect`, `GetWindowRect`,
  `ClientToScreen`, `ScreenToClient`, `IsWindowVisible`, `IsIconic`,
  `IsZoomed` and `GetWindowThreadProcessId` answer for another process's
  window.  Firefox's GPU process sizes its frames from the main process's
  window; before, it saw a 0 x 0 window and never drew, so the browser
  showed white.  The full browser now draws through the GPU process.
- **Locks that sleep**: `WaitOnAddress`, SRW locks and condition
  variables park the thread until another wakes it, the way Windows 8
  and later do: ntdll lists the waiters per address and they sleep in the
  new `NtWaitForAlertByThreadId` system call until a waker calls
  `NtAlertThreadByThreadId`.  They used to poll every 10 ms, which left
  Firefox's main thread too slow to read its input.  Also
  `SleepConditionVariableSRW`/`CS` return FALSE with `ERROR_TIMEOUT` when
  they time out, as on Windows.
- **Drawing from another thread**: `ReleaseDC` shows what was drawn at
  once even while part of the window waits for `WM_PAINT` (Firefox
  presents from its own thread while the window may never stop being
  invalidated).
- **Debugging aids**: Ctrl+Alt+F12 writes every program's threads to the
  serial log (state, last system call and its first argument, return
  addresses on the stack); the syscall trace shows the thread id
  (`[TRACE] name pid/tid`); the standard error of a detached process
  (Firefox's sandboxed children) goes to the serial log; and
  `tools/novarun.py` takes `!click X Y`.
- Not yet: a page's content (the tab area stays empty) and fetching a page
  over the network.

## ICU: .NET globalization and kernel32's locales

.NET 5 and later do their globalization through ICU when
`C:\Windows\System32\icu.dll` loads, and fall back to NLS otherwise;
until now NovaOS had no `icu.dll`, so .NET programs only knew English and
the invariant culture.  NovaOS now ships ICU the way Windows 10 does.

- **`icu.dll`** (`third_party/icu`, ICU 77.1, Unicode License v3): one
  DLL per architecture (System32 and SysWOW64) holding ICU's common and
  i18n libraries, exporting ICU's C API (1193 functions) under unversioned
  names (`ucol_open`, not `ucol_open_77`), which is what .NET looks up.
  `tools/build_icu.py` builds it from the official ICU4C source with
  MinGW-w64; the outputs are committed.
- **ICU's data** is a separate file, as on Windows:
  `C:\Windows\Globalization\ICU\icudt77l.dat`, which `icu.dll` points ICU
  at when it loads (`tools/icu/nova_icu.c`; `ICU_DATA` overrides it).  It
  is ICU's own data less legacy code-page converters, the word-break
  dictionaries, transliteration, unit names and character names (18 MB
  instead of 31).  Every culture .NET knows (1301, plus the time zones)
  gives the same names, formats and comparisons with the trimmed data as
  with the full data.
- **kernel32's locales** (`userland/kernel32/locale.c`): .NET on Windows
  also asks `GetLocaleInfoEx(name, LOCALE_SNAME)` whether Windows knows a
  culture before it uses ICU's, and kernel32 knew only English.  It now
  has the table of all 864 Windows locales (`locale_data.h`, generated by
  `tools/gen_locales.py` from .NET's MIT-licensed `IcuLocaleData.cs`):
  names, LCIDs, ANSI/OEM/Mac/EBCDIC code pages, GEOIDs, list separators,
  parents.  `LocaleNameToLCID`, `LCIDToLocaleName`, `IsValidLocale(Name)`,
  `ResolveLocaleName` and `EnumSystemLocales(A/W/Ex)` use it, and for
  locales other than English `GetLocaleInfoEx`/`W`/`A` answer from ICU:
  names in the locale's own language and in English, number and currency
  symbols and patterns, grouping, day and month names (genitive too),
  short and long dates, times, year-month and month-day formats, first day
  of the week, measurement system and paper size (the date-pattern and
  number-pattern conversions follow .NET's own, MIT).  English answers
  from the same tables as before.
- **msvcrt** exports the `_timezone`, `_daylight` and `_tzname` variables
  (MinGW-built DLLs such as `icu.dll` import them).
- **Smaller kernel image**: system files of 1 MiB or more (ICU's data and
  DLLs, NetSurf) are embedded zlib-compressed and inflated onto drive C:
  at boot, so `kernel.elf` is 27 MB rather than 48 MB with ICU in it.
- **Stack overflows** a program handles itself (the .NET runtime prints
  "Stack overflow." and exits) are logged with where they happened.
- **Tests**: `icutest` (in the CI core suite, 64- and 32-bit, 56 checks)
  loads `icu.dll` as .NET does and checks German and Japanese display
  names, numbers, currencies, dates, the Japanese calendar's era,
  collation, Turkish and German case mapping, Windows-to-IANA time zones,
  IDNA, NFC, eight threads at once, and kernel32's answers for those
  locales.  The nightly app corpus now runs .NET 10.0.12 (from NuGet) with
  `tests/dotnet/culturetest.dll`: `de-DE` gives `1.234.567,89`,
  `1.234,50 €`, `Freitag, 2. Oktober 2026`, `02.10.2026`; `ja-JP` gives
  `￥1,235`, `2026年10月2日金曜日`, `2026/10/02`, `日本語 (日本)`; German
  sorts `ä` with `a`, Japanese compares kana and widths, all through ICU.
- Not yet: `GetDateFormat`, `GetNumberFormat` and `GetCurrencyFormat`
  still format the English way for every locale, and the user's locale is
  always `en-US`.

## Older USB controllers: EHCI, OHCI and UHCI

USB devices now work on every kind of PC USB controller, not only xHCI,
and on any number of controllers at once, so machines from before about
2012 (and virtual machines that emulate only USB 1.1 or 2.0) get their
keyboards, mice, hubs and sticks too.

- **USB core** (`kernel/drivers/usb.c`, `usb_hc.h`): what used to be part
  of the xHCI driver and is the same for every controller moved into a
  core of its own: enumeration (SET_ADDRESS, or xHCI's Address Device),
  descriptors, a pipe per endpoint, offering interfaces to the hub, HID and
  mass-storage drivers, taking devices away, and the `usb` thread that
  watches the root ports.  A controller driver supplies root-port reset,
  control, bulk and interrupt IN transfers, and its own per-device and
  per-endpoint state (`UsbHcOps`).  The class drivers (`usb.h`) are
  unchanged.
- **xHCI** (`xhci.c`): its state moved into a per-controller structure,
  so every xHCI controller is started, each a bus of its own (`usb1 port
  3` in the log).  On Intel 7- to 9-series chipsets it takes the shared
  USB 2 ports over from EHCI, as Windows does.
- **EHCI** (`ehci.c`, USB 2): a queue head per endpoint, control and bulk
  on the asynchronous ring and interrupt ones on a periodic list that
  every frame points at; transfers are chains of qTDs (20 KiB each) and a
  short packet ends a bulk or interrupt transfer through an inactive
  "stop" qTD.  Full- and low-speed devices on a root port are passed to
  the companion controller that shares the port (`PORT_OWNER`); behind a
  high-speed hub they go through the hub's transaction translator (split
  transactions).  The firmware's legacy support is turned off first.
- **OHCI** (`ohci.c`, USB 1.1): an endpoint descriptor per endpoint on the
  control, bulk or interrupt list, transfers queued behind a dummy TD
  (8 KiB per TD), the data toggle kept in the ED, and a short packet in
  the middle of a bulk transfer ending it.  The controller is taken from
  the firmware's SMM driver (`OwnershipChangeRequest`).
- **UHCI** (`uhci.c`, USB 1.1): skeleton queue heads for interrupt,
  control and bulk transfers behind every frame, a queue head per
  endpoint, a TD per packet with the data toggle kept by the driver, and
  short-packet detection that ends a transfer (or moves a control
  transfer on to its status stage).  Legacy keyboard emulation is turned
  off (`USBLEGSUP`).
- All of them are polled from the timer tick like the rest of NovaOS's
  drivers (their interrupts stay off); an interrupt endpoint is polled
  each frame.  Controllers start in the order that lets EHCI hand devices
  to its companions: xHCI, EHCI, then OHCI and UHCI.  Before sleep (S3)
  every xHCI controller arms its root ports to wake the machine (Phase
  18.6's keyboard wake, now a controller operation, `prepare_sleep`);
  after it each controller is reset and its devices enumerated again.
- **Keyboard LEDs**: Num Lock, Caps Lock and Scroll Lock follow the lock
  keys (`InputLockState`, which now tracks Num and Scroll Lock too).  The
  report descriptor's LED outputs are found (the boot-protocol descriptor
  gained them), and the `usb` thread sends an output report
  (`SET_REPORT`) to every keyboard when the state changes.
- Tested in QEMU 8.2 (`-machine q35,i8042=off`, so typing goes over USB)
  with a keyboard, a tablet and a FAT stick (`dir`, `type`) on each of
  `qemu-xhci`, `usb-ehci`, `pci-ohci` and `piix3-usb-uhci` (on OHCI and
  UHCI the stick sat behind the hub QEMU adds when the root ports run
  out); an ICH9 EHCI with three
  UHCI companions (a full-speed hub with keyboard and mouse passed to a
  companion, a high-speed stick and tablet on EHCI); EHCI with an OHCI
  companion and full-speed keyboards (`usb_version=1`) unplugged and
  plugged in on other ports; two xHCI controllers with the keyboard moved
  from one to the other; devices added and removed while running on
  each; `sleeptest` on xHCI (two controllers), OHCI and ICH9 EHCI + UHCI
  with every device back after waking.  Caps Lock reached the keyboards
  as `SET_REPORT` output reports (QEMU's USB packet capture).
- Not yet: isochronous transfers (webcams, USB audio), interrupt
  endpoints are polled every frame whatever their interval, and split
  transactions through a high-speed hub's transaction translator are
  untested (QEMU's EHCI has none).  Unplugging a hub from a port that EHCI
  passed to a UHCI companion aborts QEMU 8.2 (an assertion in its USB
  core when it hands the port back); unplugging other devices there, or
  a hub elsewhere, works.

## USB hubs and report-protocol HID (Phase 18.1)

- **USB core** (`kernel/drivers/xhci.c`, `usb.h`): devices are enumerated
  on root ports and behind hubs (the slot context carries the route
  string, the root port and, for low and full speed devices behind a high
  speed hub, the transaction translator).  Every endpoint of the
  configuration gets a ring in one Configure Endpoint, then
  SET_CONFIGURATION, and each interface is offered to the class drivers,
  which open pipes: interrupt IN with a completion callback, and bulk
  transfers that wait.  A device that leaves takes everything behind it
  with it; its drivers' `gone` callbacks run on the `usb` thread.
- **Hubs** (`usbhub.c`): USB 2 and USB 3 hubs.  Ports are powered, the
  status-change endpoint says which port changed, and the `usb` thread
  reads its status, debounces, resets it and enumerates the device.
- **HID** (`usbhid.c`): report protocol.  The report descriptor is parsed
  into fields (report IDs, usage pages, arrays and bitmaps, push/pop);
  keyboards report keys held (array or bitmap), mice relative X/Y, wheel
  and buttons, tablets and touch screens absolute X/Y (scaled to the
  screen) with a button or the first contact's tip switch.  Boot-class
  devices whose report descriptor can't be used fall back to boot
  protocol.  Absolute pointers move the cursor to a position
  (`InputEvent.absolute`, `WmCursorMoveAbs`).
- Tested in QEMU (`-machine q35,i8042=off -device qemu-xhci`) with a
  `usb-hub` on port 1 holding a `usb-kbd` and a `usb-mouse`, and a
  `usb-tablet` on port 2: typing in Terminal, relative motion and absolute
  positions all work; the keyboard was unplugged and a new one added on
  another hub port, and the tablet unplugged and added on a third root
  port, with `device_del` / `device_add`.
- Not yet: multi-touch.  (Keyboard LEDs, more than one controller and the
  older UHCI/OHCI/EHCI controllers: see "Older USB controllers".)

## USB mass storage (Phase 18.2)

- **Bulk-Only Transport + SCSI** (`kernel/drivers/usbmsc.c`): INQUIRY,
  TEST UNIT READY, REQUEST SENSE, READ CAPACITY (10 and 16), READ/WRITE
  (10 and 16) and SYNCHRONIZE CACHE, with stall handling and reset
  recovery.  Each stick becomes a removable block device (`usb0`, `usb1`,
  ...); `BlockUnregister` takes it off the list when it is pulled.
- **Drives** (`kernel/fs/drives.c`): a removable disk's FAT12/16/32 and
  NTFS volumes (whole disk, MBR or GPT) are mounted as the next free drive
  letter when it arrives, and unmounted when it goes
  (`RamfsUnmountDrive`: nodes still held stay valid but read nothing).
  Fixed disks still only mount NTFS, since their FAT volumes are NovaOS's
  own.  Mounts are read-only for now.
- **File Explorer** lists the other drives (D: to Z:) under the places in
  its sidebar, names them by label in the title and breadcrumb, and goes
  back to This PC when the drive it shows is unplugged.
- Tested in QEMU with a FAT32 (MBR) `usb-storage` stick present at boot
  (`dir`, `type`, Explorer, Notepad), unplugged and plugged back in with
  `device_del` / `device_add`, and an NTFS stick added while running.

## NVMe disks (Phase 18.3)

- **Driver** (`kernel/drivers/nvme.c`): resets each NVMe controller, sets
  up an admin queue and one I/O queue pair (64 entries, polled through the
  completion phase bit), identifies the controller and its active
  namespaces, and registers each namespace with 512-byte blocks as a
  block device (`nvme0n1`, ...).  Reads and writes go through a 128 KiB
  bounce buffer described by PRP entries or a PRP list; Flush backs
  `BlockDev.flush`.  After S3 the controllers are reset and their queues
  created again.
- **Install**: NVMe disks are found with the SATA disks (`PersistInit`),
  so Setup lists them, a blank one holds drive C: in a live session, and
  NovaOS installs to them.
- Tested in QEMU/OVMF with a blank 1 GB `-device nvme` and the ISO:
  Setup installed onto `nvme0n1`, then the NVMe disk alone booted
  (OVMF's `UEFI QEMU NVMe Ctrl` entry), and a file written in Terminal was
  saved to its NOVADATA partition.

## NTFS write (Phase 18.4)

- **Writer** (`kernel/fs/ntfs.c`, NovaOS's own code: ntfs-3g and the
  Linux ntfs3 driver are GPL): rewrites a file's data (resident when it
  fits in its MFT record, else in newly allocated clusters, the old ones
  freed once the record points at the new ones), creates files and
  directories (a `$STANDARD_INFORMATION` carrying the parent's security
  id, a Win32 `$FILE_NAME`, an empty `$DATA` or `$I30` index), renames
  and moves them, and deletes files and empty directories (a file with
  other hard links only loses the name).  Directory indexes are rebuilt
  whole on each change, as a B+ tree of INDX blocks packed bottom up
  (`$UpCase` collation), in the index root alone while it fits; the
  `$INDEX_ALLOCATION` grows, and is given back when the index fits in the
  root again.  The MFT grows 64 records at a time.  The cluster and MFT
  bitmaps are kept in memory and written through; records 0-3 are
  mirrored to `$MFTMirr`.  There is no journal: `$LogFile` is emptied when
  writing starts (as `ntfsfix` does) and each call leaves the volume
  consistent.  Files in an attribute list, and compressed, sparse or
  encrypted files, are not rewritten: the first time something asks
  whether such a file is writable its record is read, and it shows as
  read-only from then on (the directory's copy of its attributes can't be
  trusted for this; ntfs-3g leaves it stale).
- **When it is writable**: a volume Windows left hibernated (a
  `hiberfil.sys` starting `hibr`, which Fast Startup leaves too), marked
  dirty, or with unfinished transactions in `$LogFile` stays read-only.
- **Drives** (`kernel/fs/drives.c`, `ramfs.c`): `RamfsSource` gained
  create, remove, rename, write and free-space calls.  Creating, deleting
  and renaming on a writable drive go to the disk at once; a file's new
  contents are written when nothing holds it any more, after a quiet
  second (`DrivesPoll`), and at shutdown (`DrivesSync` from `UmSaveAll`).
  Volume information reports the real free space and drops
  `FILE_READ_ONLY_VOLUME`.  Renames within drives D:, E:, ... are real
  renames now (they were refused as "another device", so `MoveFile`
  copied and deleted).
- **Checks**: `drivetest` now writes (small, appended and 700 KB files,
  overwrites, renames, moves, a case change, a folder of 300 files, then
  deletes) and leaves two files for the host; `scripts/check-ntfs-disk.sh`
  runs `ntfsfix -n` and `scripts/ntfs-check.py`, a chkdsk-style check of
  the cluster and MFT bitmaps against what the records own, the records'
  headers, attributes and link counts, every `$I30` index (order, leaf
  depth, VCNs, index bitmap, entries against the `$FILE_NAME`s) and
  `$LogFile`.  It reports no errors on volumes mkntfs and ntfs-3g made,
  and none after drivetest and Terminal tests (MFT growth, a 90-file
  folder split across INDX blocks and shrunk back, a USB stick written to
  and pulled out without a sync) wrote to them.
  Windows `chkdsk` has not been run on them: there is no Windows here.

## NTFS as drive C: and file ACLs (Phase 18.5)

Drive C: can now be kept on NTFS, with each file's security descriptor,
and the kernel enforces those DACLs.

- **`$Secure` writing** (`kernel/fs/ntfs.c`): `NtfsAddSecurity` stores a
  descriptor once (found again by hash and bytes), appending it to `$SDS`
  and its mirror 256 KiB on, never across a 256 KiB block, and adds it to
  the `$SII` and `$SDH` view indexes; `NtfsSetSecurityId` points a file's
  standard information at it.  The index code takes any named index now,
  view-index roots are kept small so record 9 doesn't fill, `$SDS` grows
  64 KiB at a time, allocations extend a stream's last run when they can,
  and attributes of one type are kept in name order (ntfs-3g looks them up
  that way).  `NtfsSetInfo` saves times and attribute bits and
  `NtfsLookup` finds a name in a folder.  New volumes give the root
  `CREATOR OWNER` full control, inherited, as Windows' C:\ has.
- **File security** (`kernel/fs/fsec.c`): a RamNode may carry a
  self-relative descriptor; one without inherits from the nearest folder
  above that has one (object- and container-inherit ACEs, no-propagate,
  `CREATOR OWNER`).  `FsecAccess` checks a request against the user's
  token SIDs with the file generic mapping; the owner can always read and
  change the DACL.  With no descriptor anywhere above, as on FAT, anyone
  may do anything.
- **Syscalls** (`kernel/um/um_syscall.c`): opening, creating, overwriting,
  deleting, renaming and setting basic information check the DACL (delete
  falls back to the parent's `FILE_DELETE_CHILD`; `MAXIMUM_ALLOWED` gets
  what is granted).  A descriptor given in `OBJECT_ATTRIBUTES` (from
  `CreateFile`'s or `CreateDirectory`'s `SECURITY_ATTRIBUTES`) is applied
  to the new file.  `NtQuerySecurityObject` (0x155) and
  `NtSetSecurityObject` (0x1A1, their Windows 10 1903 numbers) moved
  from ntdll into the kernel; other
  handles still get the default descriptor.  advapi32's
  `Get/SetNamedSecurityInfo`, `Get/SetSecurityInfo` and
  `Get/SetFileSecurity` are real now.
- **Persistence** (`kernel/fs/persist.c`): an NTFS volume labelled
  `NOVADATA` can hold C:.  C: is its root (on FAT it stays `\NOVA\C`),
  NovaOS's own bookkeeping goes in a hidden `$NovaOS` folder, and each
  file is saved with its times, attributes and security id (one that
  inherits gets the root's id, read back as "inherit").
- **Installer**: the confirm page asks how to keep drive C:, NTFS
  (recommended) or FAT32; NTFS data partitions have no 1 TiB cap.
- **Checks**: `acltest` gained file tests on C:\AclTest (inheritance,
  deny write and delete, rename, `MAXIMUM_ALLOWED`, the owner restoring
  access, a DACL from `SECURITY_ATTRIBUTES`) and leaves `kept.txt`, whose
  DACL a second run after a restart checks.  `scripts/ntfs-check.py`
  checks `$SII`, `$SDH` and `$SDS` against each other and attribute order;
  `check-ntfs-disk.sh` also runs `ntfssecaudit -a`.  In QEMU, NovaOS was
  installed with C: on NTFS to a blank NVMe disk and booted from it;
  `acltest` passed 33 of 33 on both boots and `kept.txt` kept its DACL.
  ntfs-check, `ntfsfix -n` and `ntfssecaudit` found no errors on the
  partition, nor on a host test volume with 5,100 descriptors.
- **Not done**: hard links (`CreateHardLink`) are still to come.

## ACPI: SCI interrupt, lid, thermal zones, wake devices, _PRT (Phase 18.6)

- **The SCI is an interrupt** (`kernel/hal/ioapic.c`): the I/O APICs come
  from the MADT, every input is masked at boot, and the SCI (FADT
  `SCI_INT`, with the MADT's interrupt source override) is routed to vector
  0x32 on the boot CPU.  Its handler runs uACPI's, which queues GPE methods
  and `Notify` handlers and wakes the `acpi` thread to run them.  The
  thread still calls the handler once a second in case an edge was missed,
  and polls every 100 ms as before when there is no I/O APIC.  The routes
  are put back after S3.  It is the only device interrupt; the drivers
  still poll.
- **PCI interrupt routing**: with an I/O APIC, `\_PIC(1)` selects APIC
  mode and the root bridge's `_PRT` is read, link devices (`PNP0C0F`)
  resolved through their `_CRS`; `AmlPciIrq` answers which GSI a pin is
  wired to, and each function on bus 0 gets it in its Interrupt Line
  register.  (QEMU's q35: 128 entries, all through links.)
- **The lid** (`PNP0C0D`): `_LID` is read at load, on `Notify 0x80` and
  after waking.  Closing it puts the machine to sleep, as Windows does by
  default.
- **Thermal zones**: `_TMP`, `_PSV`, `_HOT`, `_CRT` and `_TZP`, read every
  `_TZP` (at least every 10 s) and on `Notify 0x80`/`0x81`.  Crossing the
  passive trip point is logged (NovaOS can't throttle the CPUs yet);
  `_HOT` puts the machine to sleep and `_CRT` shuts it down, drive C: saved
  first.
- **Wake devices** (`_PRW`): the lid, power buttons and USB host
  controllers have their wake GPEs set up, and before S3 `_DSW` (or
  `_PSW`) tells them to arm.  USB keyboards that can are set for remote
  wakeup (`SET_FEATURE(DEVICE_REMOTE_WAKEUP)`), and before S3 the xHCI root
  ports enable wake on connect, disconnect and over-current, suspend
  (U3) the ports with a device and turn on PME#.  After waking the kernel
  logs which wake GPE (or the power button, or the RTC alarm) woke it.
- **For programs**: `NtPowerInformation` answers `SystemPowerCapabilities`
  (`LidPresent`, `SystemS3`, `ThermalControl`, batteries),
  `ThermalInformation` (the first zone; converted to the 32-bit layout in
  ntdll) and `LastSleepTime`/`LastWakeTime`; powrprof's
  `GetPwrCapabilities` and `CallNtPowerInformation` use them.
- **Self-test**: `tests/acpi/lid-thermal.asl` describes a lid, a thermal
  zone (40 C; passive 60, hot 90, critical 95 C) and the xHCI controller as
  a wake device, with QEMU's `pc-testdev` (ports 0xE8 and 0xE9, written
  from the QEMU monitor) as the embedded controller.  `powertest` checks
  the capabilities and readings, then asks the test to close the lid:
  NovaOS sleeps, the test opens the lid, presses a key on the USB keyboard
  and wakes the machine, and `powertest` sees `LastSleepTime` and
  `LastWakeTime` move on; then the zone is heated to 70 C (passive cooling
  on) and cooled to 45 C.  It runs in the CI boot, which now also has a
  USB keyboard on an xHCI controller (the self-tests are typed through
  it).  Also tested by hand: 91 C sleeps and 96 C shuts down.
- **Not done**: in QEMU a USB key can't wake the machine itself.  QEMU
  8.2 delivers the key to the suspended port (`xhci_wakeup`) but has no
  path from there to the platform, so the test wakes it with
  `system_wakeup` (which QEMU reports as the power button).  USB wake
  needs checking on real hardware, as do GPE block devices other than
  `\_GPE` and routing behind PCI bridges.

## HPET and one-shot/TSC-deadline timers (Phase 18.7)

- **HPET** (`kernel/hal/hpet.c`): found through the ACPI `HPET` table
  (read straight from the RSDP, before the rest of ACPI starts), its main
  counter started; it replaces the PIT as the reference that calibrates
  the TSC and the local APIC timer (the PIT stays the fallback).
- **The APIC timer is one-shot**, in TSC-deadline mode where the CPU has
  it.  Each timer interrupt re-arms it for the CPU's next 10 ms tick or
  the earliest TSC-deadline sleeper on that CPU, whichever is sooner, so
  the tick work stays at 100 Hz while sleeps end when they are due.
- **Sleeps and timed waits** (`NtDelayExecution`, `NtWaitFor*Object(s)`
  timeouts) sleep until a TSC deadline (`sched_sleep_until_tsc`) instead
  of whole 10 ms ticks.  A thread woken by its deadline goes to the front
  of its run queue and preempts a running thread of no higher priority.
- `sleeptest timer` measures how late `Sleep(1)`, `Sleep(5)` and a 1 ms
  wait timeout end, idle and with a busy thread on every CPU; it is in the
  core self-tests.  In QEMU (TCG, 2 CPUs) the 95th percentile under load
  was 0.26 ms late (it was 10 to 20 ms with the 100 Hz tick).
- QEMU emulates the TSC-deadline timer only with KVM, so the self-tests
  exercise the one-shot mode; TSC-deadline mode is untested.
- Not yet: waitable timers (`SetWaitableTimer`) still fire on the 10 ms
  tick.

## IPv6, HTTP/2 and virtio-net (Phase 18.8)

- **virtio-net** (`kernel/drivers/virtio_net.c`): virtio 1.0 network
  adapters (QEMU `virtio-net-pci`), through the modern PCI capabilities;
  one receive and one transmit queue of 64 buffers, polled like the e1000
  driver.  The network stack tries the e1000 first, then virtio-net, and
  sets either up again after sleep.
- **IPv6** in lwIP: a link-local address, SLAAC addresses from router
  advertisements, DNS servers from RDNSS (added beside DHCP's, not in their
  place: a small change to `nd6.c`), MLD, and AAAA lookups (IPv6 first
  when there is a global IPv6 address).  `ipconfig` shows the addresses,
  `ping -6`, `curl -6` and `wget -6` force IPv6 (`-4` forces IPv4), and
  `curl http://[addr]/` takes literals.
- **Winsock over IPv6**: `AF_INET6` sockets (dual-stack: IPv4 peers show
  as v4-mapped), `sockaddr_in6` in `connect`, `bind`, `accept`,
  `sendto`/`recvfrom` and the name calls; `getaddrinfo` returns IPv6 and
  IPv4 addresses (with `AI_V4MAPPED`, `AI_CANONNAME`, numeric hosts with
  `%zone`, service names), and `inet_pton`/`inet_ntop`,
  `WSAStringToAddress`/`WSAAddressToString` and `getnameinfo` handle IPv6.
  `netcat` resolves with `getaddrinfo` and takes `-4`/`-6`/`-p`.
- **Loopback** (for Firefox, whose processes talk over a socket pair):
  lwIP's loopback interface is on, so 127.0.0.1, ::1 and the machine's own
  addresses reach its own sockets (the net thread delivers them).  A
  connection is given its socket as soon as it arrives, so bytes the client
  sends before `accept` wait for it instead of being dropped, and closing a
  listener no longer trips an lwIP assertion that stopped the network.
  `getaddrinfo("localhost")` gives ::1 and 127.0.0.1 and `gethostbyname`
  127.0.0.1, without DNS.  `looptest` checks all of it in the network suite.
- **winhttp.dll** (`userland/winhttp`) became a real HTTP client: sessions,
  connections and requests, request headers, request bodies
  (`WinHttpWriteData`), `WinHttpQueryHeaders` (by index, name, number or
  date), `WinHttpQueryDataAvailable`/`WinHttpReadData`, redirects, Basic
  credentials, `WinHttpCrackUrl`/`WinHttpCreateUrl`, options and time-outs,
  and asynchronous sessions that report through the status callback.
  HTTPS goes through Schannel (secur32); with
  `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL` it offers `h2` by ALPN and speaks
  HTTP/2 through [nghttp2](https://nghttp2.org/) 1.64.0 (MIT, vendored in
  `third_party/nghttp2`) when the server picks it
  (`WINHTTP_OPTION_HTTP_PROTOCOL_USED` says which).  Each request opens its
  own connection; there is no proxy support.
- **Tests**: `tools/selftest.py --suite network` boots twice with a
  virtio-net card.  On QEMU's user network it checks `ipconfig`, `ping`,
  Winsock over IPv4 and `httptest suite` (`userland/programs/httptest.c`:
  HTTP/2 negotiated, a 300 KB body, POST, redirects, a refused untrusted
  certificate, chunked HTTP/1.1, the asynchronous API) against
  `tools/h2server.js` (Node).  On an IPv6-only network, `tools/v6peer.py`
  (a router, DNS and HTTP server reached through a QEMU datagram netdev,
  so the host needs no IPv6) checks SLAAC and RDNSS, `ping -6`, `curl -6`
  and Winsock over IPv6.
- Also fixed: the USB hot-plug thread could be enumerating when the
  machine went to sleep, so after the wake its command timed out and the
  USB keyboard did not come back (an intermittent `powertest` failure).
  Sleep now waits for it, and it stays idle until the controller is
  running again.

## Display persistence (Phase 18.9)

- **The chosen resolution survives a restart.**  Choosing a mode in
  Settings > Display, or `ChangeDisplaySettings` with
  `CDS_UPDATEREGISTRY`, saves it where Windows keeps it,
  `HKLM\SYSTEM\CurrentControlSet\Control\Video\{NovaOS-Display}\0000`
  (`DefaultSettings.XResolution`, `YResolution`, `BitsPerPel`), which
  reaches the disk with the rest of drive C:.  At boot, once drive C: and
  the registry are loaded and before the desktop starts, the kernel
  switches to that mode if the adapter has it (`[DISPLAY] Restored the
  saved mode WxH`); on another adapter without it, NovaOS stays in the
  boot mode.
- **Windows grow back.**  A window a smaller mode shrank or pushed aside
  remembers the frame it had and gets it back when a later mode has room
  for it, unless it was moved or resized in between.  Maximized windows
  already followed the work area.
- `disptest` checks both (46 checks): a 1000x640 window shrinks to fit
  800x600 and comes back to its size and place, and `CDS_UPDATEREGISTRY`
  writes the registry values.  The core self-tests save 1024x768 with
  `disptest 1024 768`, and after the suite's restart (`shutdown /r`)
  `disptest saved 1024 768` passes only if NovaOS came up in that mode.
- Not yet: a per-monitor layout (NovaOS drives one display).

## Recording: waveIn, WASAPI capture and endpoint volume

Phase 19.4.  The HD Audio driver now also programs an input stream: it
picks the codec's first input pin, preferring a microphone, then line in,
aux and CD, routes it through the mixer and selector widgets to an ADC,
and records 48 kHz 16-bit stereo into a ring the mixer thread reads every
tick.  The kernel mixer gained capture streams: each running one gets a
copy of what was recorded (the oldest frames dropped and counted when a
program falls behind), and the card records only while one runs.  Each
direction has a master volume, applied as the mixer mixes or copies.
`NtNovaAudioOpen` with its top bit set opens a capture stream, and
`NtNovaAudioCtl` gained four operations: read frames, describe the
recording device, and set and get the endpoint volume.

On top of that:
- winmm: `waveIn` (devices, capabilities, open with every callback kind,
  prepare and add buffers, start, stop, reset, position).  A thread
  converts the mixer's frames into each buffer in the program's format and
  hands it back full (`WIM_DATA`); stop hands back a partly filled one.
  The converter learned the reverse direction (any rate, 8 to 32-bit or
  float, mono as the average of both sides).
- mmdevapi: a second endpoint, the recording one ("Microphone (High
  Definition Audio)", or "Line in"), in the enumerator's lists and as the
  default `eCapture` device; its `IAudioClient` fills its buffer from a
  capture stream and `IAudioCaptureClient` hands it out ten milliseconds
  at a time, flagging a discontinuity when frames were dropped.  Both
  endpoints have `IAudioEndpointVolume` (scalar and decibel levels from
  -65.25 to 0 dB, per channel, mute, steps), kept in the kernel so every
  program sees the same volume.

To test it, `tools/novarun.py --rec FILE.wav` gives QEMU an `hda-micro`
card on a private PulseAudio server: the microphone hears FILE played over
and over into a null sink, and the speakers go to a second null sink
(saved with `--wav`).  Null sinks run on a clock, so the guest records in
real time; QEMU's ALSA backend with the file plugin delivered audio about
ten times too fast.  The core self-tests now record 3 s through `waveIn`
at 44.1 kHz mono and 3 s through WASAPI in the mix format, and
`tools/wavcheck.py --tone 523 2500` must find the 523 Hz tone in each
recording; the WASAPI test turns the recording volume down to a quarter
half way and must measure the level a quarter as loud, and `soundtest
volume` checks the volume controls on both endpoints.

## Windows Installer depth: custom actions, dialogs, shortcuts, services

Roadmap step 20.5.  Before writing anything we looked for a permissive
(MIT, BSD, zlib) Windows Installer to reuse; there is none (Wine's is
LGPL), so the engine stays NovaOS's own, with `stb_image` (public domain
or MIT) for the dialogs' pictures.

- **Custom actions that run code.**  DLL actions (types 1 and 17) run out
  of process, as on Windows: `msiexec /novaca` is a custom-action server
  that loads the DLL and calls its entry point, and a 32-bit DLL (WiX's
  `WixCA` usually is, even in x64 packages) runs in `SysWOW64`'s
  `msiexec`.  Its `MsiGetProperty`, `MsiDatabaseOpenView`,
  `MsiProcessMessage`... calls cross a pipe back to the installation.
  EXE actions (2, 18, 34, 50) start the program from the Binary table, an
  installed file, a folder or a property; type 19 shows its error; 35 and
  51 set folders and properties.  The flags are honoured: continue on
  error, asynchronous, run once, deferred with `CustomActionData`, commit
  actions after `InstallFinalize`; rollback actions are kept unused (there
  is no rollback yet).  Script actions (VBScript, JScript) and nested
  installs are logged and skipped.
- **The MSIHANDLE API** (`userland/msi/api.c`), exported at Windows'
  ordinals: records, views with an SQL engine (`sql.c`: `SELECT ... WHERE
  ... ORDER BY`, joins, `INSERT`/`UPDATE`/`DELETE`, `CREATE TABLE` and
  temporary rows through `MsiViewModify`), properties, formatted strings
  (`[Prop]`, `[#File]`, `[!File]`, `[$Comp]`, `[%ENV]`, `[1]`, `{...}`
  groups), feature and component states, target and source paths,
  `MsiDoAction`, `MsiSequence`, `MsiEvaluateCondition`, the summary
  information stream, `MsiGetMode` and `MsiSetMode`.
- **The packages' own dialogs.**  At full UI a package with an
  `InstallUISequence` shows its own wizard (`dialog.c`): the Dialog,
  Control, ControlEvent, ControlCondition, EventMapping, TextStyle,
  RadioButton, CheckBox, ListBox and ComboBox tables; text, bitmaps (BMP,
  PNG, JPEG), icons, lines and group boxes painted in place; push buttons,
  check boxes, edit and path fields, radio groups, the licence's RTF,
  progress bars, the feature tree (with check boxes), folder lists and
  combos, volume lists, list and combo boxes as real controls.  Events:
  property changes, `NewDialog`, `SpawnDialog`, `EndDialog`, `DoAction`,
  `SetTargetPath`, `Reset`, `AddLocal`, `Remove`, `SetInstallLevel`,
  `SelectionBrowse` and the folder-list events; the progress dialog
  follows `ActionText`, `ActionData` and `SetProgress`; Cancel asks first.
  The success, cancel and failure dialogs (-1, -2, -3) end it.  Packages
  without dialogs keep the progress window.
- **Shortcuts**: the Shortcut table (`IShellLink`, with arguments,
  working folder, description, show command and icons from the Icon
  table, saved under `C:\Windows\Installer\{ProductCode}`), into the
  Start menu (`C:\AppData\Roaming\Start Menu\Programs`) and the
  desktop; `msiexec /x` removes them.  shell32 gains the
  InternetShortcut class (`.url` files with `IUniformResourceLocator`,
  `IPersistFile` and its property set) that WiX's internet shortcuts use.
- **Services**: `ServiceInstall` and `ServiceControl` (install, start,
  stop, delete, on install and on uninstall), on a **service control
  manager** in `advapi32` (`service.c`).  Services live under
  `HKLM\SYSTEM\CurrentControlSet\Services` as on Windows;
  `StartService` runs the image, whose `StartServiceCtrlDispatcher` runs
  `ServiceMain` on a thread and takes controls on a pipe; `ControlService`,
  `QueryServiceStatus(Ex)`, `QueryServiceConfig(2)`, `ChangeServiceConfig(2)`,
  `EnumServicesStatus(Ex)`, `DeleteService` and the A forms work.
  `tools/msitest/make_service_package.sh` builds a package around a test
  service (`svc.c`).  Services marked automatic do not start at boot yet.
- **AppSearch** follows DrLocator (with Parent chains and Depth) and the
  Signature table (file names, minimum and maximum versions and sizes),
  besides RegLocator, whose directory and file types now check the
  signature too.
- **Smaller pieces**: `CostingComplete`, empty files missing from the
  cabinet are created, `MsiEnumRelatedProducts`, the features installed
  are remembered so maintenance runs and removals know them, and
  `ADDLOCAL`/`REMOVE` follow the dialogs' feature choices.  New:
  `activeds.dll` (ADSI; binding fails, as on a machine in no domain),
  which WiX's util custom actions import.  `advapi32` names all the usual
  well-known SIDs (`LOCAL SERVICE`, `Guests`, `Performance Log Users`...)
  and clears the last error on success where callers look at it.
- **FAT**: creating a file whose 8.3 alias was taken walked the folder
  once per `~N` tried, so a folder of a thousand similar names (CMake's
  documentation) froze the desktop while drive C: was saved.  One walk
  now marks the taken numbers.
- **Merge modules** are merged into a package's own tables when it is
  built (the `Module*` tables only record what came from where), so the
  engine installs them like any other component; none of the packages
  tested here carries one, so that is untested.
- **LZX on real packages**: 7-Zip's MSI (LZX:21) extracts byte for byte
  as `cabextract` does, and Node.js, CMake, Temurin and KeePassXC
  install.
- **`taskkill.exe`** (`/IM`, `/PID`, `/F`), which WiX's quiet-exec
  actions run to close a program before it is replaced.
- **C runtime**: MinGW programs lock a stream themselves, entering the
  critical section Microsoft's CRT keeps after each `FILE` (`_FILEX`) and
  setting a flag in `_flag` for the standard three.  msvcrt now allocates
  streams that way and keeps its flags where `_flag` sits, so a MinGW
  `fprintf` to a file no longer hangs (the test service's `ServiceMain`
  did).
- Tested in QEMU: 7-Zip and CMake through their own wizards (welcome,
  licence, options, folder, ready, progress, finish; CMake's
  `ValidatePath` and `DetectNsisOverwrite` DLL actions run from its
  dialogs); 7-Zip's two Start menu shortcuts; with `/qn`, CMake
  (`cmake --version`), Node.js (64-bit and 32-bit WiX custom actions,
  `node -e`, Start menu and internet shortcuts) and Temurin (the
  `WixRemoveFoldersEx` action, `java -version`) installed, ran and were
  removed with `/x`; KeePassXC (32-bit `WixQuietExec` running
  `taskkill`) installed and was removed, though it needs `MSVCP140.dll`
  (the Visual C++ runtime) to start; the test service installed,
  started, wrote its state, stopped and was deleted.  PowerShell 7 stops
  at its own launch condition: it wants Windows' `pwrshplugin.dll`
  (WinRM remoting), which NovaOS lacks.
- Not yet: rollback, script custom actions, nested installs, patches
  (`.msp`) and transforms (`.mst`), advertised features, services at boot.

<!-- END generated:history -->
