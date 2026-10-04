# ndrtest: rpcrt4's NDR engine, the code COM proxy/stub DLLs run on.
# ndrtestps.dll (a proxy/stub DLL made with widl) is registered with
# DllRegisterServer (NdrDllRegisterProxy), found through CoGetPSClsid and
# CoGetClassObject (NdrDllGetClassObject), and gives an interface stub
# (CStdStubBuffer, NdrStubCall2) and a stubless proxy joined by a channel
# that hands each request to the stub: strings, arrays, structs, unique
# pointers, hyper and floating-point arguments, [out] data the proxy
# allocates, a delegated base interface, server failures and crashes.
# 64- and 32-bit.
DOC = '`ndrtest` (rpcrt4\'s NDR engine: stubless proxies, NdrStubCall2, CStdStubBuffer and the NdrDll* entry points, 64- and 32-bit)'
TESTS = [
    Test('ndrtest x64', 'ndrtest', [r'ndrtest: \d+ passed, 0 failed \(64-bit\)'], timeout=120),
    Test('ndrtest x86', r'C:\Programs\x86\ndrtest.exe', [r'ndrtest: \d+ passed, 0 failed \(32-bit\)'], timeout=120),
]
