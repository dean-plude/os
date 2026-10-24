/* NovaOS: MinGW-w64's rpcdce.h, whose RPC string type RPC_WSTR is always
 * unsigned short *; the Windows SDK's is wchar_t * when the build asks for
 * it (RPC_USE_NATIVE_WCHAR, as Microsoft builds XAML's telemetry, which
 * keeps UuidToString's string in a std::wstring: components/Telemetry's
 * XamlTraceSession.cpp).  With RPC_USE_NATIVE_WCHAR, MinGW-w64's
 * declarations keep their own type and RPC_WSTR is wchar_t *, with the
 * wide functions XAML calls taking it too. */
#pragma once
#if defined(RPC_USE_NATIVE_WCHAR) && defined(__cplusplus)
#define RPC_WSTR nova_mingw_RPC_WSTR
#include_next <rpcdce.h>
#undef RPC_WSTR
typedef wchar_t *RPC_WSTR;
extern "C++" {     /* (MinGW-w64's rpc.h includes this inside extern "C") */
inline RPC_STATUS UuidToStringW(const UUID *uuid, RPC_WSTR *string)
{
    return UuidToStringW(uuid, reinterpret_cast<nova_mingw_RPC_WSTR *>(string));
}
inline RPC_STATUS RpcStringFreeW(RPC_WSTR *string)
{
    return RpcStringFreeW(reinterpret_cast<nova_mingw_RPC_WSTR *>(string));
}
}
#else
#include_next <rpcdce.h>
#endif
