/* msvcrt_internal.h — helpers shared by the msvcrt sources (not exported) */
#pragma once

/* posix.c: descriptor table */
int   __nova_fd_new(void *handle, int owns);    /* new descriptor; -1 on failure */
void *__nova_fd_handle(int fd);                 /* INVALID_HANDLE_VALUE if bad */
int   close(int fd);
void  __nova_set_errno_win32(void);
