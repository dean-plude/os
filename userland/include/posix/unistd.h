/* unistd.h — POSIX process/file helpers (NovaOS) */
#pragma once
#include <sys/types.h>
_NOVA_BEGIN
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
_CRTIMP int     access(const char *path, int mode);
_CRTIMP int     unlink(const char *path);
_CRTIMP int     rmdir(const char *path);
_CRTIMP char   *getcwd(char *buf, size_t size);
_CRTIMP int     chdir(const char *path);
_CRTIMP int     close(int fd);
_CRTIMP ssize_t read(int fd, void *buf, size_t n);
_CRTIMP ssize_t write(int fd, const void *buf, size_t n);
_CRTIMP off_t   lseek(int fd, off_t off, int whence);
_CRTIMP ssize_t pread(int fd, void *buf, size_t n, off_t off);
_CRTIMP ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
_CRTIMP int     dup(int fd);
_CRTIMP int     dup2(int fd, int fd2);
_CRTIMP unsigned sleep(unsigned sec);
_CRTIMP int     usleep(useconds_t usec);
_CRTIMP pid_t   getpid(void);
_CRTIMP int     isatty(int fd);
_CRTIMP int     ftruncate(int fd, off_t len);
_CRTIMP int     fsync(int fd);
_CRTIMP long    sysconf(int name);
_CRTIMP int     gethostname(char *name, size_t len);
_CRTIMP uid_t   getuid(void);
_CRTIMP uid_t   geteuid(void);
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
_NOVA_END
