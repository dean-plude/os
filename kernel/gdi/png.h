/*
 * png.h — PNG decoder (for icons)
 *
 * Decodes a PNG image (every colour type and bit depth, interlaced or not,
 * with tRNS transparency) to 32-bit pixels 0xAARRGGBB with straight alpha.
 * The zlib stream is inflated by a small built-in decoder.
 */

#pragma once

#include "../include/types.h"

#define PNG_MAX_DIM    4096
#define PNG_MAX_PIXELS (4 * 1024 * 1024)      /* 16 MB decoded */

/* True if @data starts with the PNG signature. */
bool PngIsPng(const void *data, size_t len);

/* Decode @data.  On success *out is a kmalloc'd w*h array (free it with
 * kfree) and *w, *h are set; returns false for anything malformed. */
bool PngDecode(const void *data, size_t len, UINT32 **out, int *w, int *h);

/* Inflate a zlib stream (RFC 1950/1951) into @dst; returns the number of
 * bytes produced, or -1 on error or if the output would exceed @cap. */
long ZlibInflate(const void *src, size_t len, void *dst, size_t cap);
