#pragma once
#include <_nova.h>
_NOVA_BEGIN
_CRTIMP int isalpha(int c);
_CRTIMP int isdigit(int c);
_CRTIMP int isalnum(int c);
_CRTIMP int isspace(int c);
_CRTIMP int isupper(int c);
_CRTIMP int islower(int c);
_CRTIMP int isxdigit(int c);
_CRTIMP int ispunct(int c);
_CRTIMP int isprint(int c);
_CRTIMP int isgraph(int c);
_CRTIMP int iscntrl(int c);
_CRTIMP int isblank(int c);
_CRTIMP int toupper(int c);
_CRTIMP int tolower(int c);
#define isascii(c) ((unsigned)(c) < 0x80)
#define toascii(c) ((c) & 0x7F)
_NOVA_END
