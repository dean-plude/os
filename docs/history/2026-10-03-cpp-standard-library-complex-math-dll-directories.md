## The C++ standard library, C99 complex math and DLL directories

Three gaps the Python and Qt programs ran into, all closed.  Before
writing anything we looked for permissively licensed code to reuse, and
two of the three are existing open-source code shipped as OS components.

- **`msvcp140.dll`, the C++ standard library, is Microsoft's own STL.**
  Microsoft publishes the STL that Visual Studio ships
  ([microsoft/STL](https://github.com/microsoft/STL), Apache-2.0 WITH
  LLVM-exception, the licence family of LLVM's libc++).  It is the only
  source whose classes have the layouts, names and exports MSVC-built
  programs import, so NovaOS builds it rather than an imitation (Wine's
  `msvcp` is LGPL).  `third_party/msstl` holds the `vs-2022-17.13`
  release's headers and sources; `userland/msvcp140/build.py` compiles
  them with clang in MSVC mode against MinGW-w64's headers, plus a few
  shim headers for the VC runtime's private ones (`userland/msvcp140/inc`).
  The DLL exports all 1515 names of Microsoft's current `msvcp140.dll`.
  The satellites come from the same sources: `msvcp140_1` (`std::pmr`),
  `msvcp140_atomic_wait` (atomic waits, parallel algorithms, `<syncstream>`,
  the time-zone database over `icu.dll`) and `msvcp140_codecvt_ids`;
  `msvcp140_2` (the special math functions) is not built yet, as it needs
  Boost.Math.  Each module also gets its own `operator new`/`delete` and
  start-up code (`start.cpp`, `new.cpp`), and the import library's static
  part (`msvcprt_static.lib`: `std::filesystem`, `to_chars`, `std::format`,
  `shared_mutex`...).  One clang difference needed care: under
  `#pragma init_seg(compiler)`, clang files the initializers of exported
  objects (`std::cerr`) apart from the static ones that use them, so the
  build renames those sections and each source's initializers keep their
  order.
- **C99 complex math in `ucrtbase`/`msvcrt`**: musl's `src/complex` (MIT,
  next to the musl libm already there): `cabs`, `carg`, `cexp`, `clog`,
  `csqrt`, `cpow`, the trigonometric and hyperbolic functions, their `f`
  and `l` forms, `creal`/`cimag`, plus the UCRT's own `_Cbuild`,
  `_Cmulcc`, `_Cmulcr`, `norm` and their float and long double forms.
  clang's `_Complex` passes and returns exactly as MSVC's `_Dcomplex`
  and `_Fcomplex` structures do, so the functions take what MSVC-built
  callers hand them.  `_cprintf`, `_cputs` and the UCRT's
  `__conio_common_vcprintf` family write to the console.
- **`AddDllDirectory`, `RemoveDllDirectory` and `SetDllDirectory` are
  real.**  The loader searches the `SetDllDirectory` folder and then every
  added folder after the importing module's own folder, for
  `LoadLibrary` and for imports alike, so Python's
  `os.add_dll_directory` (NumPy's `numpy.libs`) works.  The folders live
  in the process (`kernel/um/um.c`, through `NtNovaLoadDll`).
- **For KeePassXC**: a `d3d11.dll` of NovaOS's own (Qt's GUI library
  imports it): without DXVK it answers `DXGI_ERROR_UNSUPPORTED`, as
  Windows does with no Direct3D 11 device, and with the App Store's DXVK,
  which now installs its `d3d11.dll` as `d3d11_dxvk.dll` (like its
  `dxgi`), it passes the calls on.  New: `winscard.dll` (no smart card
  service: `SCARD_E_NO_SERVICE`), `HidD_GetFeature`/`SetFeature`,
  `GetCharABCWidthsI`, `UpdateLayeredWindowIndirect`,
  `Shell_NotifyIconGetRect`, `WTSQuerySessionInformationW`,
  `CheckRemoteDebuggerPresent`, `SetSearchPathMode`, `WSAHtonl` and
  friends, and `CommandLineToArgvW` through the `shcore` API set.  32-bit
  `vcruntime140` and `ucrtbase` now export `__std_terminate`, `_setjmp`,
  `_except_handler3`, `_wcstoui64` and the like under their real names
  (the linker had dropped a leading underscore).
- **Tests**: `stltest` (28 checks: strings, containers, streams, locales,
  exceptions, threads, `std::async`, atomic waits, `pmr`,
  `std::filesystem`, `std::format`, `std::regex`) and `rttest` (30:
  complex math, conio, DLL directories) pass 64- and 32-bit and run in the
  CI core suite.  Python 3.14 with the NumPy 2.5.3 wheel, on NovaOS's own
  runtime DLLs, imports NumPy and prints the same complex `exp`, `sqrt`,
  `log`, `sin`, `tanh`, `power`, `linalg.inv`, `fft` and `eigvals` results
  as NumPy on Linux.  KeePassXC 2.7.11 starts and shows its main window
  and first-run dialog.
- Not yet: Qt's widgets draw their shapes and icons in KeePassXC but not
  their text; `msvcp140_2.dll`.
