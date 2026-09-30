/*
 * ramfs.h — in-memory filesystem backing drive C: for the desktop apps
 *
 * Drive C: lives in memory; it is seeded at boot, and fs/persist.c saves
 * what changes to a FAT volume on disk and restores it at the next boot.
 *
 * Paths use '\' (or '/'), are case-insensitive, and may be absolute
 * ("C:\Documents\a.txt", "\Documents") or relative to a directory, with
 * "." and ".." components.
 */

#pragma once

#include "../include/types.h"

#define RAMFS_NAME_MAX   48
#define RAMFS_PATH_MAX   256
#define RAMFS_FILE_MAX   (64u * 1024u * 1024u)  /* largest file (netsurf.exe, downloaded installers) */

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
    UINT8           pflags;       /* RAMFS_F_*: origin and unsaved changes */
    UINT32          attrs;        /* FILE_ATTRIBUTE_READONLY/HIDDEN/SYSTEM (Windows programs) */
    UINT64          ctime, mtime; /* created, last written: 100 ns units since 1601 (UTC) */
} RamNode;

/* Change tracking, for saving drive C: to disk (fs/persist.c).  Nodes
 * remember where they came from and what changed since the last save;
 * RAMFS_F_SUB marks every ancestor of a changed node. */
#define RAMFS_F_SEALED   0x01     /* installed from the OS image: never saved */
#define RAMFS_F_SEED     0x02     /* a starter file: deleting it must be remembered */
#define RAMFS_F_DIRTY    0x04     /* contents (file) or existence (dir) to save */
#define RAMFS_F_DIRTYDIR 0x08     /* entries added or removed */
#define RAMFS_F_SUB      0x10     /* something below changed */

typedef enum {
    RAMFS_TRACK,                  /* normal operation: record changes */
    RAMFS_SEEDING,                /* new nodes are starter files */
    RAMFS_INSTALLING,             /* new nodes are sealed system files */
    RAMFS_LOADING,                /* restoring saved files: nothing to record */
} RamfsMode;

void     RamfsSetMode(RamfsMode mode);
/* Changes recorded so far (a counter that only grows). */
UINT32   RamfsChanges(void);
/* Called with the path of a starter file (RAMFS_F_SEED) that goes away. */
void     RamfsSetRemovedHook(void (*fn)(const char *path));
/* Called with the directory whose entries (or files) changed, for
 * programs watching directories (FindFirstChangeNotification). */
void     RamfsSetChangeHook(void (*fn)(RamNode *dir));

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

/* The clock new and written files are stamped with (100 ns since 1601);
 * until it is set, files get no times. */
void RamfsSetClock(UINT64 (*now)(void));
/* Record that @n's times or attributes changed (it is saved again) */
void RamfsMarkChanged(RamNode *n);
