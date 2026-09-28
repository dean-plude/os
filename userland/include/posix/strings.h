/* strings.h — BSD string helpers (NovaOS) */
#pragma once
#include <string.h>
_NOVA_BEGIN
#define bzero(p, n)      memset((p), 0, (n))
#define bcopy(s, d, n)   memmove((d), (s), (n))
#define bcmp(a, b, n)    memcmp((a), (b), (n))
#define index            strchr
#define rindex           strrchr
_CRTIMP int ffs(int i);
_NOVA_END
