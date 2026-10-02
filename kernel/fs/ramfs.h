/*
 * ramfs.h — in-memory filesystem backing drive C: for the desktop apps
 *
 * Drive C: lives in memory; it is seeded at boot, and fs/persist.c saves
 * what changes to a FAT volume on disk and restores it at the next boot.
 *
 * Paths use '\' (or '/'), are case-insensitive, and may be absolute
 * ("C:\Documents\a.txt", "\Documents") or relative to a directory, with
 * "." and ".." components.
 *
 * Other drives (D:, E:, ...) are read-only volumes on disk mounted into
 * the same tree of nodes: a drive's root has no parent, a directory's
 * entries are read from the volume the first time something looks in it,
 * and a file's contents when it is opened (RamfsLoad), then dropped again
 * when nothing holds the file any more.  Nothing on them can be changed.
 */

#pragma once

#include "../include/types.h"

#define RAMFS_NAME_MAX   256       /* a path component, as on Windows (255 + NUL) */
#define RAMFS_PATH_MAX   256
#define RAMFS_FILE_MAX   (256u * 1024u * 1024u) /* largest file (netsurf.exe, downloaded installers) */

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
    int             pins;         /* readers of data outside the lock (RamfsPin) */
    UINT8           pflags;       /* RAMFS_F_*: origin and unsaved changes */
    UINT32          attrs;        /* FILE_ATTRIBUTE_READONLY/HIDDEN/SYSTEM (Windows programs) */
    UINT64          ctime, mtime; /* created, last written: 100 ns units since 1601 (UTC) */
    UINT8           xflags;       /* RAMFS_X_*: on a mounted volume */
    char            drive;        /* a drive's root: its letter ('C' for the root of C:) */
    UINT64          xref;         /* on a mounted volume: the node's number there */
} RamNode;

#define RAMFS_X_EXTERN   0x01     /* on a mounted (read-only) volume */
#define RAMFS_X_LISTED   0x02     /* directory: its entries were read */
#define RAMFS_X_LOADED   0x04     /* file: @data holds its contents */

/* A mounted volume, as its file system reads it */
typedef struct {
    char   name[RAMFS_NAME_MAX];
    bool   dir;
    UINT64 ref;                   /* the entry's number on the volume */
    UINT64 size, ctime, mtime;
    UINT32 attrs;
} RamfsExtEntry;

typedef struct {
    /* Calls @add for each entry of directory @ref; false on a read error */
    bool (*list)(void *vol, UINT64 ref, bool (*add)(const RamfsExtEntry *e, void *ctx), void *ctx);
    /* The real size of file @ref, and reading @len bytes of it from @off */
    bool (*size)(void *vol, UINT64 ref, UINT64 *size);
    bool (*read)(void *vol, UINT64 ref, UINT64 off, void *buf, UINT64 len);
} RamfsSource;

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

/* Mount the volume @vol (@total_bytes large), read by @src, as drive
 * @letter (D-Z); its root directory is @root_ref.  NULL if the letter is taken or memory is out. */
RamNode *RamfsMountDrive(char letter, const RamfsSource *src, void *vol, UINT64 root_ref, const char *label,
                         UINT64 total_bytes);
/* The root of drive @letter, or NULL */
RamNode *RamfsDriveRoot(char letter);
/* The drives there are: bit 0 for A:, bit 2 for C:, ... */
UINT32   RamfsDriveMask(void);
/* The volume label and file system name of the drive @n is on ("NTFS"),
 * its size in bytes; false for drive C: */
bool     RamfsDriveInfo(const RamNode *n, const char **label, const char **fs, UINT64 *total);
/* The letter of the drive @n is on ('C', 'D', ...) */
char     RamfsDriveLetter(const RamNode *n);
/* Nodes on a mounted volume can't be changed */
bool     RamfsReadOnly(const RamNode *n);
/* Read what a node on a mounted volume needs from the disk: a directory's
 * entries, a file's contents.  True at once for other nodes.  False on a
 * read error, or a file too large to hold (RAMFS_FILE_MAX). */
bool     RamfsLoad(RamNode *n);

/* Resolve a path relative to `cwd` (NULL = root).  NULL if not found. */
RamNode *RamfsResolve(RamNode *cwd, const char *path);
/* The same without loading anything (no change to the tree, so readers may
 * look up side by side): *@unloaded is set, and NULL returned, when it
 * would have to read a mounted volume's directory. */
RamNode *RamfsLookup(RamNode *cwd, const char *path, bool *unloaded);

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
 * underneath its holder.  NULL is ignored.  (Both are atomic: readers
 * sharing the lock take and drop references side by side, though only
 * for files of drive C:, whose contents never unload.) */
void     RamfsRef(RamNode *node);
void     RamfsUnref(RamNode *node);

/* Hold a file and its contents still (loading them, see RamfsLoad: @data
 * is NULL if that fails): while pinned, @data stays where it
 * is and unchanged (writes and resizes fail, as Windows refuses writes to
 * a file mapped as an image), so it can be read without the lock that
 * guards the file system (the program loader does).  Pinning also refs. */
void     RamfsPin(RamNode *file);
void     RamfsUnpin(RamNode *file);

/* Absolute path, e.g. "C:\Documents\a.txt" (root is "C:\"). */
void     RamfsPath(const RamNode *node, char *buf, int cap);

int      RamfsCount(const RamNode *dir);

/* The clock new and written files are stamped with (100 ns since 1601);
 * until it is set, files get no times. */
void RamfsSetClock(UINT64 (*now)(void));
/* Record that @n's times or attributes changed (it is saved again) */
void RamfsMarkChanged(RamNode *n);
