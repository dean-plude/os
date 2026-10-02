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
- Not yet: SVG and IPv6.
