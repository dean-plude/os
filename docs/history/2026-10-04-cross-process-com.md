## COM between processes

COM servers that run as their own program (`LocalServer32`), and calls
from one process into an object in another, are how installers and
updaters such as Microsoft Edge Update talk to their own background
servers.  NovaOS's COM worked inside one process only; with rpcrt4's NDR
engine in place, ole32 and oleaut32 now carry calls between processes.
None of it is taken from Wine (whose ole32 is LGPL); the wire layout of
an object reference (`OBJREF`, "MEOW") follows Microsoft's published
DCOM specification.

- **The standard marshaler** (`userland/ole32/marshal.c`):
  `CoMarshalInterface` writes an `OBJREF` naming the exporting process,
  the object and an interface identifier (IPID); the exporter keeps a stub
  manager per object with an interface stub per interface (from the
  interface's proxy/stub DLL, found through `CoGetPSClsid`).
  `CoUnmarshalInterface` gives a proxy manager per object, the identity
  `IUnknown`, with an interface proxy per interface and remote
  `QueryInterface`, `AddRef` and `Release`; an object coming back to its
  own process unmarshals to the object itself.  Custom marshalers,
  table marshaling, `CoReleaseMarshalData`, `CoDisconnectObject`,
  `CoGetStandardMarshal` and `CoGetStdMarshalEx` are there too.
- **The channel**: each process listens on a named pipe; a call is a
  request with the IPID, method number and NDR data, answered on the
  same connection.  The server runs each request on its own thread and
  hands it to the object's apartment; a single-threaded apartment gets it
  through its hidden window's message queue.  A single-threaded apartment
  waiting for a reply keeps taking incoming calls, so a server can call
  back into an object of the client while the client waits.
- **Local servers**: `CoCreateInstance` and `CoGetClassObject` with
  `CLSCTX_LOCAL_SERVER` connect to a running server's class or start the
  `LocalServer32` program with `-Embedding` and wait for its
  `CoRegisterClassObject`.  `REGCLS_SUSPENDED`, `CoResumeClassObjects`,
  `CoAddRefServerProcess` and `CoReleaseServerProcess` work as on
  Windows, so servers can exit once their last object is gone.
- **oleaut32**: `BSTR_User*`, `VARIANT_User*` and `LPSAFEARRAY_User*`
  (and their 64-bit forms) marshal Automation types for MIDL-generated
  proxies, and the `IDispatch` proxy/stub (`PSDispatch` and
  `PSOAInterface` for `IDispatch` itself) carries `GetIDsOfNames` and
  `Invoke` with by-reference arguments and `EXCEPINFO`.  oleaut32 also
  exports 51 more functions it already had under their Windows ordinals
  (`VarBstrCmp`, ordinal 314, stopped Edge Update's install).
- **kernel32**: a delay-load failure (`0xC06D007E`/`0xC06D007F`) is named
  in the serial log, DLL and function, before the exception is raised.
- **The self-test** `comoop` (`tests/selftest/core/152-comoop.py`) runs a
  64- and a 32-bit client, each against a 64- and a 32-bit `comserver.exe`
  started on demand: NDR calls, a dual interface with `BSTR` and `VARIANT`
  arguments, callbacks into the client, `IDispatch::Invoke` with every
  kind of argument, server shutdown and the class factory path.

Edge Update's silent install of the WebView2 runtime now runs to the end
and stops at its own check that the runtime's package carries Microsoft's
signature.  Not there yet: pinging and cleaning up after a client process
that dies, the typelib marshaler (`PSOAInterface` for interfaces other
than `IDispatch`), handler marshaling and the free-threaded marshaler.
