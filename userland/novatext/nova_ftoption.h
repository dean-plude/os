/* FreeType's options for novatext.dll (FT_CONFIG_OPTIONS_H): the defaults,
 * without zlib (no gzip module, so no WOFF 1 fonts) */
#include <freetype/config/ftoption.h>
#undef FT_CONFIG_OPTION_USE_ZLIB
