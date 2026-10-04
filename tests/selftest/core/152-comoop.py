# comoop: COM between processes.  CoCreateInstance(CLSCTX_LOCAL_SERVER)
# starts comserver.exe from its LocalServer32 key with -Embedding, the
# server registers its class with CoRegisterClassObject, and calls go
# through ole32's standard marshaler (OBJREFs) and named-pipe channel,
# ndrtestps.dll's NDR proxies and stubs and oleaut32's IDispatch
# proxy/stub with BSTR, VARIANT and SAFEARRAY arguments, a callback into
# the client's apartment, EXCEPINFO, server shutdown on the last release
# and the class object path.  Each bitness talks to both servers.
DOC = '`comoop` (cross-process COM: LocalServer32 activation, the standard marshaler and RPC channel, the IDispatch proxy/stub, BSTR/VARIANT marshalling; 64- and 32-bit clients and servers)'
TESTS = [
    Test('comoop x64', 'comoop', [r'comoop: \d+ passed, 0 failed \(64-bit\)'], timeout=300),
    Test('comoop x86', r'C:\Programs\x86\comoop.exe', [r'comoop: \d+ passed, 0 failed \(32-bit\)'], timeout=300),
]
