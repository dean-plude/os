/*
 * ndrtestps.dll — a proxy/stub DLL for ndrtest's self-test of rpcrt4's NDR
 * engine, made the way MIDL makes one: the proxy and stub tables come from
 * widl (Wine's MIDL) and this file is the "dlldata.c" that lists them.
 * Everything else (the class object, DllRegisterServer, the proxies and
 * stubs themselves) is rpcrt4's NdrDll* and Ndr* code, so the DLL exercises
 * those entry points as a real proxy DLL (psmachine.dll, say) would.
 *
 * INdrTest (idl/ndrtest.idl) derives from INdrBase (idl/ndrbase.idl), kept
 * in a separate proxy file so INdrTest is delegated: its base methods go
 * through a forwarding proxy and a delegating stub.
 *
 * To regenerate the tables after changing the IDL (Debian/Ubuntu's
 * mingw-w64-tools has widl), from idl/:
 *   for n in ndrbase ndrtest; do
 *     x86_64-w64-mingw32-widl -h -H ../$n.h $n.idl
 *     x86_64-w64-mingw32-widl -m64 -Oif -p $n.idl && mv ${n}_p.c ../${n}_p64.inc
 *     x86_64-w64-mingw32-widl -m32 -Oif -p $n.idl && mv ${n}_p.c ../${n}_p32.inc
 *   done
 * then make the headers' "#include <ndrbase.h>" and "<ndrunk.h>" quoted.
 */
#include <windows.h>
#define REGISTER_PROXY_DLL
#define PROXY_CLSID_IS { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xb0, 0xff } }
#include <rpcproxy.h>

EXTERN_PROXY_FILE(ndrbase)
EXTERN_PROXY_FILE(ndrtest)

PROXYFILE_LIST_START
    REFERENCE_PROXY_FILE(ndrbase),
    REFERENCE_PROXY_FILE(ndrtest),
PROXYFILE_LIST_END

DLLDATA_ROUTINES(aProxyFileList, GET_DLL_CLSID)
