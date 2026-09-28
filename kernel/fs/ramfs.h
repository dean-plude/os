/*
 * ramfs.h — in-memory filesystem backing drive C: for the desktop apps
 *
 * There is no disk driver yet and the bootloader provides no initrd, so
 * the desktop (Terminal, File Explorer, Notepad) works on this RAM disk.
 * Contents are seeded at boot and lost on reboot.
 *
 * Paths use '\' (or '/'), are case-insensitive, and may be absolute
 * ("C:\Documents\a.txt", "\Documents") or relative to a directory, with
 * "." and ".." components.
 */

#pragma once

#include "../include/types.h"

#define RAMFS_NAME_MAX   48
#define RAMFS_PATH_MAX   256
#define RAMFS_FILE_MAX   (8u * 1024u * 1024u)   /* largest file (netsurf.exe, downloads) */

typedef struct RamNode {
    char            name[RAMFS_NAME_MAX];
    bool            dir;
    struct RamNode *parent;
    struct RamNode *child;        /* first child (directories only) */
    struct RamNode *next;         /* next sibling; dirs first, then by name */
    char           *data;         /* file contents (not NUL-terminated) */
    UINT32          size;
    UINT32          cap;          /* bytes allocated for data (>= size) */
    int             refs;         /* holders (open windows, shell cwd) */
} RamNode;

void     RamfsInit(void);
RamNode *RamfsRoot(void);

/* Resolve a path relative to `cwd` (NULL = root).  NULL if not found. */
RamNode *RamfsResolve(RamNode *cwd, const char *path);

/* Child named `name` in `dir` (case-insensitive), or NULL. */
RamNode *RamfsFind(RamNode *dir, const char *name);

/* Create `name` in `dir`.  If it already exists with the same kind it is
 * returned; with a different kind, NULL.  Names may not contain '\' '/'. */
RamNode *RamfsCreate(RamNode *dir, const char *name, bool is_dir);

/* Replace a file's contents.  False if too large or out of memory. */
bool     RamfsWrite(RamNode *file, const char *data, UINT32 len);

/* Write @len bytes at @off, growing the file (zero-filled) as needed.
 * False if the result would exceed RAMFS_FILE_MAX or memory runs out. */
bool     RamfsWriteAt(RamNode *file, UINT32 off, const void *data, UINT32 len);
/* Set a file's length (truncate or zero-extend). */
bool     RamfsResize(RamNode *file, UINT32 len);

/* Delete a file or an empty directory.  Fails for the root and for nodes
 * that are in use (see RamfsRef). */
bool     RamfsDelete(RamNode *node);

/* Move/rename @node to @name in @dir.  An existing file of that name is
 * replaced if @replace (and not in use); directories are never replaced.
 * False on a bad name, a clash, or moving a directory into itself. */
bool     RamfsRename(RamNode *node, RamNode *dir, const char *name, bool replace);

/* Mark a node as held (e.g. shown in a window) so it cannot be deleted
 * underneath its holder.  NULL is ignored. */
void     RamfsRef(RamNode *node);
void     RamfsUnref(RamNode *node);

/* Absolute path, e.g. "C:\Documents\a.txt" (root is "C:\"). */
void     RamfsPath(const RamNode *node, char *buf, int cap);

int      RamfsCount(const RamNode *dir);
