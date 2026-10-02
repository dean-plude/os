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
