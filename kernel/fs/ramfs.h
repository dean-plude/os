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
 * Other drives (D:, E:, ...) are volumes on disk mounted into the same
 * tree of nodes: a drive's root has no parent, a directory's entries are
 * read from the volume the first time something looks in it, and a file's
 * contents when it is opened (RamfsLoad), then dropped again when nothing
 * holds the file any more.  A volume whose file system can write (its
 * RamfsSource has the write calls) can be changed: creating, deleting,
 * renaming and linking go to the disk at once, a file's new contents when
 * nothing holds it any more or at the next RamfsFlush.
 *
 * A file may have several names (hard links, CreateHardLink): one RamNode
 * per name, in a ring, sharing the contents and details.  On drive C: they
 * are kept across restarts (fs/persist.c: as NTFS hard links, or listed in
 * \NOVA\LINKS.TXT on FAT).
 */

#pragma once

#include "../include/types.h"

#define RAMFS_NAME_MAX   256       /* a path component, as on Windows (255 + NUL) */
#define RAMFS_PATH_MAX   256
#define RAMFS_FILE_MAX   (2048u * 1024u * 1024u) /* largest file: game installers' data files (GOG Galaxy's offline setup is 342 MB) */

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
    UINT8          *sd;           /* its security descriptor (self-relative), or NULL: */
    UINT32          sdlen;        /*   inherited from the nearest directory above with one (fs/fsec.c) */
    struct RamNode *link;         /* a file with several names (hard links): the next one, around a ring; else NULL. */
    UINT8           lent;         /* @data may be lent to a save (RamfsLend) */
} RamNode;                        /*   Names share data, size, cap, attrs, times, sd and the RAMFS_X_* state */

#define RAMFS_X_EXTERN   0x01     /* on a mounted (read-only) volume */
#define RAMFS_X_LISTED   0x02     /* directory: its entries were read */
#define RAMFS_X_LOADED   0x04     /* file: @data holds its contents */
#define RAMFS_X_DIRTY    0x08     /* file: @data is newer than the disk */
#define RAMFS_X_NOWRITE  0x10     /* file: its volume can't rewrite it (compressed, sparse, encrypted) */
#define RAMFS_X_CHECKED  0x20     /* file: the volume was asked whether it can rewrite it */

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
    /* Writing (all NULL on a read-only volume): create @name in directory
     * @dir, remove @ref's name @name from @dir, move @ref's name @old in
     * @dir to @to as @name, replace @ref's contents */
    bool (*create)(void *vol, UINT64 dir, const char *name, bool is_dir, UINT64 *ref);
    bool (*remove)(void *vol, UINT64 dir, UINT64 ref, const char *name);
    bool (*rename)(void *vol, UINT64 dir, UINT64 ref, const char *old, UINT64 to, const char *name);
    bool (*write)(void *vol, UINT64 ref, const void *data, UINT64 len);
    UINT64 (*free_bytes)(void *vol);
    bool (*can_write)(void *vol, UINT64 ref);                    /* (NULL: every file) */
    /* Hard links (NULL on a file system without them): give file @ref the
     * name @name in @dir too; how many names @ref has */
    bool (*link)(void *vol, UINT64 ref, UINT64 dir, const char *name);
    UINT32 (*links)(void *vol, UINT64 ref);
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
                         const char *fs, UINT64 total_bytes);
/* The volume behind drive @letter went away (a USB stick was pulled): the
 * letter is free again.  Nodes still held from it stay valid but empty and
 * read nothing more (RamfsDetached).  Returns its volume pointer. */
void    *RamfsUnmountDrive(char letter);
/* @n belongs to a drive that has been unmounted */
bool     RamfsDetached(const RamNode *n);
/* The root of drive @letter, or NULL */
RamNode *RamfsDriveRoot(char letter);
/* The drives there are: bit 0 for A:, bit 2 for C:, ... */
UINT32   RamfsDriveMask(void);
/* The volume label and file system name of the drive @n is on ("NTFS", "FAT32"),
 * its size in bytes; false for drive C: */
bool     RamfsDriveInfo(const RamNode *n, const char **label, const char **fs, UINT64 *total);
/* The free bytes on the drive @n is on (0 for a read-only one) */
UINT64   RamfsDriveFree(const RamNode *n);
/* The letter of the drive @n is on ('C', 'D', ...) */
char     RamfsDriveLetter(const RamNode *n);
/* @n is on a mounted volume that can't be changed (or was unmounted) */
bool     RamfsReadOnly(const RamNode *n);
/* Write the mounted volumes' changed files to their disks.  False if one
 * could not be written. */
bool     RamfsFlush(void);
/* Files changed on mounted volumes and not yet written, and a counter of
 * changes to them that only grows */
UINT32   RamfsExtDirty(void);
UINT32   RamfsExtChanges(void);
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
/* The same with @buf (from kmalloc), which the file takes over: no copy.
 * On failure @buf is still the caller's. */
bool     RamfsWriteOwned(RamNode *file, char *buf, UINT32 len);

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

/* Give @file another name, @name in @dir (a hard link, on the same drive):
 * the new node, which shares the file's contents and details.  NULL on a
 * bad name, a clash, a directory, another drive or a read-only volume.
 * Deleting one name leaves the file to the others. */
RamNode *RamfsLink(RamNode *file, RamNode *dir, const char *name);
/* How many names @n has (1 without hard links) */
int      RamfsLinks(const RamNode *n);
/* One node for the file whatever name it was reached by: the file's identity */
const RamNode *RamfsFileId(const RamNode *n);
/* The next name of @n's file (around the ring, back to @n), for walking its names */
RamNode *RamfsNextLink(RamNode *n);

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
UINT64 RamfsNow(void);                          /* 0 until it is set */
/* Record that @n's times, attributes or security descriptor changed (it is saved again) */
void RamfsMarkChanged(RamNode *n);
/* Saving C: (fs/persist.c) writes files' contents to the disk after letting
 * go of the file-system lock, without copying them.  RamfsLend (with the
 * lock held exclusively) returns @f's contents and keeps those bytes where
 * they are and unchanged until RamfsGiveBackAll: a write to @f meanwhile
 * gives it a copy first, and contents replaced or deleted meanwhile are
 * freed by RamfsGiveBackAll, not at once. */
const char *RamfsLend(RamNode *f);
void     RamfsGiveBackAll(void);
/* Mark @n with @flags (RAMFS_F_DIRTY, _DIRTYDIR) again: a save could not
 * write it (fs/persist.c).  No change notification: nothing changed. */
void RamfsMarkUnsaved(RamNode *n, UINT8 flags);
