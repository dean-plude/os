/* getopt.h — command-line option parsing (NovaOS) */
#pragma once
#include <_nova.h>
_NOVA_BEGIN
struct option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};
#define no_argument       0
#define required_argument 1
#define optional_argument 2
_CRTIMP extern char *optarg;
_CRTIMP extern int   optind, opterr, optopt;
_CRTIMP int getopt(int argc, char *const argv[], const char *optstring);
_CRTIMP int getopt_long(int argc, char *const argv[], const char *optstring,
                        const struct option *longopts, int *longindex);
_NOVA_END
