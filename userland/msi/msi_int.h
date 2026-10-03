/*
 * msi_int.h — Windows Installer for NovaOS: internal interfaces
 *
 * The package readers (compound file, database tables, cabinets) work on
 * memory buffers and use only the C library, so they build and test on a
 * host as well; install.c and ui.c are the Win32 part.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Compound file (OLE structured storage): the .msi container
 * ----------------------------------------------------------------------- */
typedef struct {
    const uint8_t *data;
    size_t         size;
    int            sector_shift;      /* 9 or 12 */
    uint32_t       nfat;              /* FAT sectors */
    uint32_t       dir_start, minifat_start, nminifat, difat_start, ndifat;
    uint32_t       mini_cutoff;
    uint32_t      *fat;               /* the whole FAT, sector -> next */
    uint32_t       fat_entries;
    uint32_t      *minifat;
    uint32_t       minifat_entries;
    uint8_t       *ministream;        /* the root entry's stream */
    size_t         ministream_size;
    int            nentries;          /* directory entries */
    int            root;              /* the storage this Cfb looks in (0: the file's root) */
    bool           borrowed;          /* a storage opened inside another Cfb: shares its tables */
} Cfb;

bool  cfb_open(Cfb *c, const void *data, size_t size);
void  cfb_close(Cfb *c);
/* Read a stream of this storage by decoded name; malloc'd, NULL if missing */
void *cfb_read(const Cfb *c, const char *name, size_t *size);
/* A storage inside this one (by decoded name) as a Cfb of its own; it
 * shares @parent's memory, so it is used while @parent is open */
bool  cfb_open_storage(const Cfb *parent, const char *name, Cfb *out);
/* The streams and storages of this storage, by decoded name */
void  cfb_list(const Cfb *c, void (*fn)(void *ctx, const char *name, bool is_stream), void *ctx);

/* -----------------------------------------------------------------------
 * The database: string pool and tables
 * ----------------------------------------------------------------------- */
#define MSI_STRING   0x0800
#define MSI_NULLABLE 0x1000
#define MSI_KEY      0x2000

typedef struct {
    char    *name;
    uint16_t type;
    int      width;                   /* bytes per row in the stream */
} MsiColumn;

typedef struct {
    char      *name;
    int        ncols;
    MsiColumn *cols;
    int        nrows;
    uint8_t   *stream;                /* column-major raw rows */
    size_t     stream_size;
} MsiTable;

struct MsiTemp;                       /* sql.c: rows and tables added at run time */
struct MsiOverlay;                    /* msidb.c: storages whose streams count as the database's */

typedef struct {
    Cfb       cfb;
    char    **strings;                /* by id; [0] = "" */
    int       nstrings;
    int       strref_size;            /* 2 or 3 */
    int       codepage;
    MsiTable *tables;
    int       ntables;
    struct MsiTemp *temp;
    struct MsiOverlay *overlays;      /* transforms' and patches' storages, newest last */
    int       noverlays;
    int      *hash, hash_cap;         /* the pool by text, for adding strings */
} MsiDb;

/* A stream column ('v0': string type, no length): the cell refers to the
 * stream named "Table.Key" */
#define MSI_IS_BINARY(type) (((type) & ~(MSI_NULLABLE | MSI_KEY | 0x4000)) == 0x0900)

bool        msidb_open(MsiDb *db, const void *data, size_t size);
void        msidb_close(MsiDb *db);
MsiTable   *msidb_table(MsiDb *db, const char *name);
int         msidb_col(const MsiTable *t, const char *name);       /* -1 if none */
/* A cell as text: strings by value, integers formatted, "" for null.
 * @buf must hold 16 bytes for integers; strings return the pool's copy. */
const char *msidb_str(const MsiDb *db, const MsiTable *t, int row, int col, char *buf);
/* An integer cell; @null tells null apart from 0 */
int         msidb_int(const MsiDb *db, const MsiTable *t, int row, int col, bool *null);
/* Row whose column @col equals @value, from @from; -1 if none */
int         msidb_find(const MsiDb *db, const MsiTable *t, int col, const char *value, int from);

/* A compound file in memory (malloc'd @data, owned from here), shared by
 * the storages opened in it: a transform, a patch */
typedef struct MsiFile MsiFile;
MsiFile    *msifile_load(void *data, size_t size);
const Cfb  *msifile_cfb(const MsiFile *f);
void        msifile_release(MsiFile *f);
/* The streams of storage @storage (NULL: the root) of @f count as @db's */
bool        msidb_add_streams(MsiDb *db, MsiFile *f, const char *storage);
/* A stream of the database, or of a transform or patch applied to it */
void       *msidb_read_stream(const MsiDb *db, const char *name, size_t *size);
/* Apply the transform in storage @storage (NULL: the file is one) to the
 * tables; @suppress: the MSITRANSFORM_ERROR_* conditions to ignore.
 * Returns 0, or 1624 (ERROR_INSTALL_TRANSFORM_FAILURE) with @err set. */
int         msidb_apply_transform(MsiDb *db, MsiFile *f, const char *storage, int suppress, char *err, int errcap);
/* A summary information property of storage @c (strings into @str, numbers into @ival) */
bool        msi_suminfo_get(const Cfb *c, int pid, char *str, int cap, int *ival);

/* -----------------------------------------------------------------------
 * Records and SQL views (sql.c), the data behind MSIHANDLEs
 * ----------------------------------------------------------------------- */
enum { MSIF_NULL, MSIF_INT, MSIF_STR, MSIF_STREAM };

typedef struct {
    int      type;
    int      i;
    char    *s;                       /* MSIF_STR: UTF-8, malloc'd */
    uint8_t *data;                    /* MSIF_STREAM */
    size_t   size, pos;
} MsiField;

typedef struct MsiRec {
    int       n;                      /* fields 1..n; f[0] is the format field */
    MsiField *f;
    void     *view;                   /* the view it was fetched from */
    int       src[4];                 /* source row in each joined table */
} MsiRec;

MsiRec     *msirec_new(int n);
void        msirec_free(MsiRec *r);
void        msirec_clear(MsiRec *r);
void        msirec_set_str(MsiRec *r, int i, const char *s);   /* NULL/"" = null */
void        msirec_set_int(MsiRec *r, int i, int v);            /* MSI_NULL_INTEGER = null */
void        msirec_set_stream(MsiRec *r, int i, const void *data, size_t size);
bool        msirec_is_null(const MsiRec *r, int i);
int         msirec_int(const MsiRec *r, int i);                 /* MSI_NULL_INTEGER for null/non-number */
const char *msirec_str(const MsiRec *r, int i, char *buf);      /* buf: 16 bytes for integers */
MsiRec     *msirec_copy(const MsiRec *r);
#define MSI_NULL_INTEGER ((int)0x80000000)

typedef struct MsiView MsiView;
/* Errors are Win32 codes: 0, ERROR_BAD_QUERY_SYNTAX (1615), ERROR_INVALID_TABLE (1628)... */
int      msisql_open(MsiDb *db, const char *sql, MsiView **out, char *err, int errcap);
int      msisql_execute(MsiView *v, const MsiRec *params);
int      msisql_fetch(MsiView *v, MsiRec **out);               /* 259 (no more items) at the end */
int      msisql_colinfo(MsiView *v, bool types, MsiRec **out);
int      msisql_modify(MsiView *v, int mode, MsiRec *rec);
void     msisql_close(MsiView *v);
void     msisql_free_temp(MsiDb *db);
/* Primary key columns of a table: a record of their names, field 0 the table */
int      msisql_primary_keys(MsiDb *db, const char *table, MsiRec **out);
bool     msisql_table_exists(MsiDb *db, const char *table);

/* -----------------------------------------------------------------------
 * Cabinets
 * ----------------------------------------------------------------------- */
typedef struct CabFile {
    char     name[260];
    uint32_t size;
    uint32_t folder_off;              /* offset in the folder's uncompressed data */
    int      folder;                  /* index, or -1 continued from the previous cabinet */
    bool     continued_next;          /* continues in the next cabinet */
    uint16_t date, time, attribs;
} CabFile;

typedef struct {
    uint32_t data_off;                /* first CFDATA */
    uint16_t ndata;
    uint16_t compress;                /* typeCompress */
} CabFolder;

typedef struct {
    const uint8_t *data;
    size_t         size;
    int            nfolders, nfiles;
    CabFolder     *folders;
    CabFile       *files;
    uint8_t        reserve_data;      /* cbCFData */
    char           next[256];         /* next cabinet name, "" if none */
    char           prev[256];
} Cab;

bool cab_open(Cab *c, const void *data, size_t size);
void cab_close(Cab *c);

/* Decompressing a folder in order, block by block.  The caller feeds the
 * uncompressed bytes to files; a folder that continues in the next
 * cabinet is resumed with cab_folder_continue(). */
typedef struct {
    const Cab *cab;
    int        folder;
    int        method;                /* 0 none, 1 MSZIP, 3 LZX */
    uint32_t   next_data;             /* offset of the next CFDATA */
    int        blocks_left;
    /* decompressor state */
    uint8_t   *window;                /* history (32 KB for MSZIP, the LZX window) */
    uint32_t   window_size, window_pos;
    void      *lzx;                   /* lzx.c state */
    uint8_t   *out;                   /* one block's output (<= 32768) */
    uint32_t   out_len;
    char       error[96];
} CabReader;

bool     cab_reader_start(CabReader *r, const Cab *cab, int folder);
/* Continue the same folder in the next cabinet (its first folder) */
bool     cab_reader_continue(CabReader *r, const Cab *next);
/* Decompress the next block into r->out (r->out_len bytes); false when the
 * folder's blocks in this cabinet are used up or on error (r->error) */
bool     cab_reader_next(CabReader *r);
void     cab_reader_end(CabReader *r);

/* MSZIP: inflate one block with @hist bytes of history before @out */
int      mszip_block(const uint8_t *in, uint32_t in_len, uint8_t *window,
                     uint32_t window_size, uint32_t *window_pos,
                     uint8_t *out, uint32_t out_cap, char *err);
/* LZX */
void    *lzx_init(int window_bits, char *err);
void     lzx_reset(void *st);
/* Decompress one frame (block of @out_len uncompressed bytes) */
int      lzx_block(void *st, const uint8_t *in, uint32_t in_len, uint8_t *out,
                   uint32_t out_len, char *err);
void     lzx_free(void *st);

/* -----------------------------------------------------------------------
 * Conditions and formatted strings (install.c supplies the property lookup)
 * ----------------------------------------------------------------------- */
typedef const char *(*MsiPropFn)(void *ctx, const char *name);
/* Evaluate an MSI condition; empty condition is true */
bool msi_condition(const char *cond, MsiPropFn prop, void *ctx);
