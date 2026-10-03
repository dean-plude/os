/* expat_config.h for NovaOS (written for this tree; upstream generates it
 * with CMake or configure).  NetSurf's SVG support (libsvgtiny, through
 * libdom's XML binding) is expat's only user. */
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

#define BYTEORDER 1234
#define HAVE_MEMMOVE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1

/* The hash salt comes from the time and the process id (no getrandom or
 * arc4random in NovaOS's C library) */
#define XML_POOR_ENTROPY 1

#endif
