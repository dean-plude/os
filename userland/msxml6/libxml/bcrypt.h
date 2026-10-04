/* the one CNG call libxml2 makes (dict.c seeds its hash with it) */
#pragma once
#ifndef BCRYPT_USE_SYSTEM_PREFERRED_RNG
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#endif
#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s) (((LONG)(s)) >= 0)
#endif
__declspec(dllimport) LONG __stdcall BCryptGenRandom(void *alg, unsigned char *buf, ULONG len, ULONG flags);
