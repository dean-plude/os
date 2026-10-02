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
- Not yet: .NET has no ICU (globalization works through NLS for English
  and invariant cultures).  (MSI custom actions run since "Windows
  Installer depth".)
