/* iconv.h — character set conversion (NovaOS: UTF-8/16/32, Latin-1,
 * Windows-1252, ASCII) */
#pragma once
#include <stddef.h>
#include <_nova.h>
_NOVA_BEGIN
typedef void *iconv_t;
_CRTIMP iconv_t iconv_open(const char *to, const char *from);
_CRTIMP size_t  iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft);
_CRTIMP int     iconv_close(iconv_t cd);
_NOVA_END
