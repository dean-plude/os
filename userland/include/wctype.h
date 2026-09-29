/* wctype.h — wide character classes (ASCII and Latin-1 classified) */
#pragma once
#include <wchar.h>
_NOVA_BEGIN
typedef unsigned short wctype_t;
_CRTIMP int iswalpha(wint_t c);
_CRTIMP int iswdigit(wint_t c);
_CRTIMP int iswalnum(wint_t c);
_CRTIMP int iswspace(wint_t c);
_CRTIMP int iswupper(wint_t c);
_CRTIMP int iswlower(wint_t c);
_CRTIMP int iswpunct(wint_t c);
_CRTIMP int iswprint(wint_t c);
_CRTIMP int iswgraph(wint_t c);
_CRTIMP int iswcntrl(wint_t c);
_CRTIMP int iswxdigit(wint_t c);
_CRTIMP int iswblank(wint_t c);
_CRTIMP int iswctype(wint_t c, wctype_t type);
_CRTIMP wctype_t wctype(const char *name);
_CRTIMP wint_t towlower(wint_t c);
_CRTIMP wint_t towupper(wint_t c);
_NOVA_END
