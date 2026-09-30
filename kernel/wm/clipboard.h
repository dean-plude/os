/*
 * clipboard.h — the system clipboard, shared by every program and the
 * built-in apps
 */
#pragma once
#include "../include/types.h"

#define CLIP_CF_TEXT         1
#define CLIP_CF_OEMTEXT      7
#define CLIP_CF_UNICODETEXT 13
#define CLIP_CF_HDROP       15
#define CLIP_CF_LOCALE      16
#define CLIP_NAME_MAX       60

typedef struct { UINT32 fmt; char name[CLIP_NAME_MAX]; UINT32 size; } ClipEntry;

/* A new clipboard content: forget everything (@owner: a window handle) */
void   ClipEmpty(UINT64 owner);
/* Add one format; @data is kmalloc'd and belongs to the clipboard now.
 * Registered formats (0xC000 and up) are named by @name. */
bool   ClipPut(UINT32 fmt, const char *name, UINT8 *data, UINT32 size);
/* Copy a format out (text formats are converted into each other); its
 * size, or -1 if there is no such format */
int    ClipGet(UINT32 fmt, const char *name, UINT8 *out, UINT32 cap);
/* The formats on the clipboard (text ones it can convert included) */
int    ClipList(ClipEntry *out, int max);
UINT32 ClipSequence(void);
UINT64 ClipOwner(void);

/* For the built-in apps: text as UTF-8 */
void   ClipSetText(const char *utf8, UINT32 len);
/* A kmalloc'd UTF-8 copy of the clipboard's text (NUL-terminated), or NULL */
char  *ClipGetText(UINT32 *len);
/* Files (CF_HDROP): paths like "C:\dir\file" */
void   ClipSetFiles(const char *const *paths, int n);
/* The i-th file on the clipboard into @out; false past the end */
bool   ClipGetFile(int i, char *out, int cap);
