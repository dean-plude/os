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
