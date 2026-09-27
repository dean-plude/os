#pragma once
#include <_nova.h>
_NOVA_BEGIN
_CRTIMP int *_errno(void);
#define errno (*_errno())
_NOVA_END
#define EPERM   1
#define ENOENT  2
#define EINTR   4
#define EIO     5
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EPIPE   32
#define EDOM    33
#define ERANGE  34
#define ENOSYS  40
#define ENOTEMPTY 41
#define EILSEQ  42
