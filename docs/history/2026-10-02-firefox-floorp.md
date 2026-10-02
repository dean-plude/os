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

