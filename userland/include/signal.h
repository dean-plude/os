#pragma once
#include <_nova.h>
_NOVA_BEGIN
typedef int sig_atomic_t;
typedef void (*__sighandler_t)(int);
#define SIGINT  2
#define SIGILL  4
#define SIGFPE  8
#define SIGSEGV 11
#define SIGTERM 15
#define SIGABRT 22
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)
_CRTIMP __sighandler_t signal(int sig, __sighandler_t fn);
_CRTIMP int raise(int sig);
_NOVA_END
