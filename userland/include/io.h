/* io.h — low-level file descriptors, MSVC names (NovaOS) */
#pragma once
#include <_nova.h>
#include <stddef.h>
_NOVA_BEGIN
_CRTIMP int       _open(const char *path, int flags, ...);
_CRTIMP int       _close(int fd);
_CRTIMP int       _read(int fd, void *buf, unsigned n);
_CRTIMP int       _write(int fd, const void *buf, unsigned n);
_CRTIMP long      _lseek(int fd, long off, int whence);
_CRTIMP long long _lseeki64(int fd, long long off, int whence);
_CRTIMP int       _access(const char *path, int mode);
_CRTIMP int       _unlink(const char *path);
_CRTIMP int       _isatty(int fd);
_CRTIMP int       _dup(int fd);
_CRTIMP int       _pipe(int *fds, unsigned size, int mode);
_CRTIMP int       _dup2(int fd, int fd2);
_NOVA_END
