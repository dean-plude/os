# NovaOS — Windows-Compatible Operating System

A clean-room, from-scratch x86_64 operating system designed to run native
Windows executables (PE32+) without emulation.

## Status: Phase 9.5 — a real web browser (NetSurf) running as a Windows program in user mode

**What works:**

### Phase 1 — Boot & Kernel Foundation
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

### Phase 2 — NT Kernel Personality
- Object Manager (`ObCreateObject`, `ObReferenceObject`, `ObDereferenceObject`, handle tables)
- Security Reference Monitor (`SeInitialize`, `SeCreateSystemToken`, ACL/ACE stubs)
- Process Manager (`EPROCESS`/`ETHREAD`/`KPROCESS`/`KTHREAD`, `PsCreateSystemProcess`, `PsCreateSystemThread`)
- Configuration Manager (registry hive bootstrap, `CmInitialize`)
- Syscall dispatcher: INT 0x2E (legacy) + SYSCALL/SYSRET MSR path (`KiSystemCall64`)
- NT syscall table with ~40 entries (NtAllocateVirtualMemory, NtCreateFile, etc.)

### Phase 3 — I/O, Sections, and PE Loader
- I/O Manager (`IoCreateDevice`, `IoCreateFile`, IRP dispatch: Create/Read/Write/QueryInfo)
- Virtual Memory Areas (`VMA_SPACE`, `VmaAllocate`, `VmaFree`, `VmaProtect` per-process)
- Section Objects (`MmCreateImageSection`, `MmMapImageView`, anonymous sections)
- PE32+ Loader (`LdrLoadImage`): header validation, section mapping, base relocation, import resolution
- Built-in stub DLLs: ntdll, kernel32, msvcrt, user32 (syscall thunks for import resolution)
- User-mode process creation (`PsCreateUserProcess` → IRETQ into ring-3 via `PsUserThreadEntry`)

### Phase 4 — VFS & InitRD
- VFS abstraction layer (`VfsMount`, `VfsOpen`, `VfsRead`, `VfsReadDir`, `VfsStat`)
- NT path alias resolution (`\??\C:\`, `\SystemRoot`, `\DosDevices\C:` → first mount)
- CPIO newc InitRD driver (`InitrdMount`): parses in-memory CPIO archive, mounts as `\Device\InitRD`
- Boot protocol v2: bootloader passes `initrd_base`/`initrd_size` in `BootInfo`
- File I/O via IRP dispatch: `NtReadFile`, `NtWriteFile` fully wired through VFS to InitRD

### Phase 5 — Process Isolation & User-Mode Foundation
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

### Phase 6 — Full User-Mode Foundation
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

### Phase 7 — GUI, Window Manager & Desktop Shell
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

### Phase 8 — Interactive Desktop
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

### Phase 8 (cont.) — Graphics & Working Desktop
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
  there is no disk, and there are no image icons.

### Phase 8 (cont.) — Networking
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
- Not yet: IPv6 and HTTP/2.  (The web browser arrived in Phase 9.5.)

### Phase 9 — Windows programs (ring 3)
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

### Phase 9.5 — The NetSurf web browser
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
  message queue.
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
- Not yet: JavaScript (NetSurf's duktape engine is left out), SVG, IPv6,
  HTTP keep-alive (one connection per request), and window resizing.

### Desktop UX refresh
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
  shows its program's first icon), and animated cursors.

## Quick Start

```bash
# Install prerequisites
sudo apt install cmake nasm clang lld llvm qemu-system-x86 ovmf mtools dosfstools xorriso

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
cmake --build . --target run
```

The first build compiles the NetSurf browser (about 800 files; later builds
reuse the cached objects). Set `NOVA_NO_NETSURF=1` in the environment to
leave the browser out.

### Bootable ISO

A ready-to-boot UEFI ISO (`nova.iso`) is committed at the repository root and
can be regenerated from a freshly built bootloader + kernel:

```bash
scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf
```

The ISO is El Torito **UEFI / no-emulation**: its EFI System Partition holds
`\EFI\BOOT\BOOTX64.EFI` (the bootloader) and `\EFI\NOVA\kernel.elf`. Boot it in
QEMU with OVMF firmware:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 512 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -cdrom nova.iso
```

It boots through every phase to the desktop on the GOP framebuffer (verified
under OVMF at 2560×1600).

On macOS with Homebrew QEMU, the UEFI firmware ships with QEMU:

```bash
FW="$(brew --prefix qemu)/share/qemu/edk2-x86_64-code.fd"
qemu-system-x86_64 -machine q35 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file="$FW" \
  -cdrom nova.iso -serial stdio
```

## Phase Roadmap

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Boot & kernel foundation | ✅ **Done** |
| 2 | NT kernel personality (Object Manager, syscalls, registry) | ✅ **Done** |
| 3 | I/O subsystem (IRP, PE loader, section objects) | ✅ **Done** |
| 4 | VFS + InitRD (virtual file system, in-memory ramdisk) | ✅ **Done** |
| 5 | Process isolation (per-process CR3, KPCR, PEB/TEB, SWAPGS) | ✅ **Done** |
| 6 | Full user-mode (SYSCALL thunk pages, Win32 helpers, CSRSS) | ✅ **Done** |
| 7 | GUI (window manager, GDI, desktop shell) | ✅ **Done** |
| 8 | Interactive desktop (PS/2 input, cursor, preemptive sched, RTC) | ✅ **Done** |
| 8.5 | Networking (e1000/e1000e, lwIP, DHCP, DNS, HTTP tools) | ✅ **Done** |
| 8.6 | HTTPS (Mbed TLS: TLS 1.3/1.2, Mozilla roots, certutil), HTTP/1.1 keep-alive | ✅ **Done** |
| 9 | Native user-mode PE execution: loader, TEB/PEB, threads, TLS, SEH, DllMain, sockets (ws2_32), GUI (user32/gdi32) | ✅ **Done** |
| 9.5 | NetSurf web browser (HTTP/HTTPS fetcher, window surface, TrueType text, POSIX C runtime) | ✅ **Done** |
| 10 | Win32 GUI subsystem (win32k, user32/gdi32, real HWNDs) | 🔄 Planned |

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the full plan toward running native
Windows executables (Phases 8–15) and the chosen compatibility strategy.

## Architecture

```
os/
├── include/              # Shared headers (boot_protocol.h, types.h)
├── bootloader/           # UEFI bootloader (PE32+ EFI application)
│   ├── include/efi.h     # Minimal UEFI API headers
│   └── src/              # main.c, elf_loader.c, paging.c, console.c
├── kernel/               # NT-style kernel (freestanding ELF64)
│   ├── arch/x86_64/      # GDT, IDT, APIC, paging, CPU intrinsics
│   ├── mm/               # PMM (bitmap), VMM (slab), VMA, section objects
│   ├── hal/              # Serial, framebuffer, PS/2 input, CMOS RTC
│   ├── ke/               # Kernel executive: main, scheduler, printf, KPCR, syscall
│   ├── ob/               # Object Manager (handles, reference counting)
│   ├── ps/               # Process Manager (EPROCESS, ETHREAD, PEB, TEB, CSRSS)
│   ├── se/               # Security Reference Monitor
│   ├── cm/               # Configuration Manager (registry)
│   ├── io/               # I/O Manager (IRP, device objects, file objects)
│   ├── fs/               # VFS layer + InitRD CPIO driver
│   ├── ldr/              # PE32+ loader + user-mode SYSCALL thunk generator
│   ├── gdi/              # GDI software renderer (2D rasterizer)
│   ├── wm/               # Window manager + desktop shell
│   ├── um/               # User-mode programs: processes, threads, PE loader, NT services,
│   │                     #   SEH delivery, sockets, GUI windows, consoles
│   ├── net/              # lwIP port, HTTP client, TLS (Mbed TLS)
│   ├── apps/             # Built-in desktop apps (Terminal, Notepad, Settings, ...)
│   ├── lib/              # Freestanding string/memory library
│   └── linker.ld         # Kernel linker script
├── userland/             # Windows SDK subset built with clang/lld-link:
│   │                     #   ntdll, kernel32, msvcrt, ws2_32, user32, gdi32,
│   │                     #   crt0, tlssup, headers, sample programs
│   └── netsurf/          # NetSurf port: fetcher, window surface, fonts, JPEG
├── third_party/          # lwIP, Mbed TLS, NetSurf + libraries, stb, fonts
├── tools/                # Host build tools (build_userland.py, build_netsurf.py, mkfont)
├── cmake/                # Cross-compilation toolchain files
├── scripts/              # build.sh, run-qemu.sh, create-disk.sh
└── docs/                 # Architecture & build documentation
```

## Key Design Decisions

- **NT ABI compatibility**: syscall numbers, NTSTATUS codes, UNICODE_STRING, and structure
  offsets match Windows 10 1903 x64 so that real ntdll.dll stubs can be used unmodified.
- **Clean-room**: no GPL code in the operating system itself; all NT API
  implementations are written from scratch using public Microsoft documentation
  and reverse-engineering references.  The one GPL component is the NetSurf
  browser, a separate program (see [License](#license)).
- **Single-CPU (Phase 5–7)**: KPCR is statically allocated for the boot CPU; SMP
  requires one KPCR per logical processor (later phase).
- **Software-only GDI**: Phase 7 rendering is a pure CPU rasterizer writing the GOP
  linear framebuffer — no GPU/2D-accel driver. It draws directly to VRAM (no back
  buffer yet); double-buffering arrives once a second VRAM mapping exists.
- **No image decoder yet**: application icons in the shell are rendered as rounded,
  brand-colored tiles with short labels — a faithful stand-in until a PNG/ICO
  decoder lands. App tiles, live tiles, and the dock are drawn from primitives.
- **Kernel-helper syscalls (0x01F0–0x01FF)**: reserved range at the top of the 512-entry
  NT dispatch table; used only internally by our user-mode stub pages — not part of the
  Windows NT ABI and invisible to real Windows binaries.
- **CSRSS Phase 6 scope**: the kernel-side shim accepts process registrations and
  returns STATUS_SUCCESS to all `CsrClientCallServer` calls; real LPC port objects
  and console server come in Phase 8.
- **Page table isolation**: each user process has its own PML4; kernel entries (256–511)
  are shared; user entries (0–255) are snapshotted at process creation.

## License

NovaOS is MIT licensed. The operating system (kernel, bootloader, system
DLLs, C runtime, desktop and apps) contains no GPL code; bundled third-party
code keeps its own permissive licence (lwIP: BSD 3-clause; Mbed TLS:
Apache-2.0; Inter and Cascadia Mono: SIL OFL 1.1; DejaVu Sans Mono:
Bitstream Vera licence; stb_truetype/stb_image: public domain or MIT).
All Win32 API implementations are clean-room based on public Microsoft
documentation, ReactOS reference, and Wine source study (but independently
written).

**NetSurf** (`third_party/netsurf/netsurf`) is licensed under the **GNU GPL
version 2**; its libraries are MIT, zlib and libpng licensed (see
`third_party/netsurf/NOVA-VENDOR.txt`).  `netsurf.exe` — NetSurf linked with
the NovaOS glue in `userland/netsurf` (MIT, GPL-compatible) — is a separate
program distributed under the GPL-2.0, with its complete source in this
repository; the kernel image merely carries it as a file for drive C:.
Build with `NOVA_NO_NETSURF=1` for an image without it.
