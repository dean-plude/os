/* string.h shim for lwIP and BearSSL: the kernel's freestanding string library */
#pragma once
#include "../../lib/string.h"
#ifdef NOVA_BEARSSL
/* BearSSL defines its own constant-time MIN/MAX functions */
#undef MIN
#undef MAX
#endif
