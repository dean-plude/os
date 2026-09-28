/*
 * jpeg_stb.c — image/jpeg for NetSurf on NovaOS, decoded by stb_image
 *
 * NetSurf's own JPEG handler (content/handlers/image/jpeg.c) is written
 * against libjpeg, which NovaOS doesn't carry; this one keeps its shape —
 * the header is read when the data arrives (for the size), and pixels are
 * decoded on demand through the image cache — with stb_image doing the
 * decoding (baseline and progressive JPEG).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/bitmap.h"
#include "content/llcache.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/image_cache.h"
#include "image/jpeg.h"

#define MIN_JPEG_SIZE 20            /* shorter than any real JPEG header */

static nserror nsjpeg_create(const content_handler *handler, lwc_string *imime_type,
                             const struct http_parameter *params, llcache_handle *llcache,
                             const char *fallback_charset, bool quirks, struct content **c)
{
    struct content *jpeg = calloc(1, sizeof(struct content));
    if (jpeg == NULL) return NSERROR_NOMEM;
    nserror error = content__init(jpeg, handler, imime_type, params, llcache, fallback_charset, quirks);
    if (error != NSERROR_OK) {
        free(jpeg);
        return error;
    }
    *c = jpeg;
    return NSERROR_OK;
}

/* Decode the whole image into a new bitmap (image cache callback) */
static struct bitmap *jpeg_cache_convert(struct content *c)
{
    size_t size;
    const uint8_t *data = content__get_source_data(c, &size);
    if (data == NULL || size < MIN_JPEG_SIZE || size > 0x7fffffff) return NULL;

    int w, h, n;
    unsigned char *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (rgba == NULL) {
        NSLOG(netsurf, INFO, "JPEG decode failed: %s", stbi_failure_reason());
        return NULL;
    }
    struct bitmap *bitmap = guit->bitmap->create(w, h, BITMAP_OPAQUE);
    uint8_t *pixels = bitmap ? guit->bitmap->get_buffer(bitmap) : NULL;
    if (pixels == NULL) {
        if (bitmap) guit->bitmap->destroy(bitmap);
        stbi_image_free(rgba);
        return NULL;
    }
    size_t rowstride = guit->bitmap->get_rowstride(bitmap);
    for (int y = 0; y < h; y++)
        memcpy(pixels + rowstride * (size_t)y, rgba + (size_t)y * w * 4, (size_t)w * 4);
    stbi_image_free(rgba);

    bitmap_format_to_client(bitmap, &(bitmap_fmt_t) { .layout = BITMAP_LAYOUT_R8G8B8A8 });
    guit->bitmap->modified(bitmap);
    return bitmap;
}

/* All data is in: read the header for the size and hand over to the cache */
static bool nsjpeg_convert(struct content *c)
{
    size_t size;
    const uint8_t *data = content__get_source_data(c, &size);
    int w = 0, h = 0, n = 0;
    if (data == NULL || size < MIN_JPEG_SIZE || size > 0x7fffffff ||
        !stbi_info_from_memory(data, (int)size, &w, &h, &n)) {
        union content_msg_data msg_data;
        msg_data.errordata.errorcode = NSERROR_UNKNOWN;
        msg_data.errordata.errormsg = "Not a valid JPEG image";
        content_broadcast(c, CONTENT_MSG_ERROR, &msg_data);
        return false;
    }
    c->width = w;
    c->height = h;
    c->size = (size_t)w * h * 4;

    image_cache_add(c, NULL, jpeg_cache_convert);

    char *title = messages_get_buff("JPEGTitle", nsurl_access_leaf(llcache_handle_get_url(c->llcache)),
                                    c->width, c->height);
    if (title != NULL) {
        content__set_title(c, title);
        free(title);
    }
    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");
    return true;
}

static nserror nsjpeg_clone(const struct content *old, struct content **newc)
{
    struct content *jpeg_c = calloc(1, sizeof(struct content));
    if (jpeg_c == NULL) return NSERROR_NOMEM;
    nserror error = content__clone(old, jpeg_c);
    if (error != NSERROR_OK) {
        content_destroy(jpeg_c);
        return error;
    }
    if ((old->status == CONTENT_STATUS_READY || old->status == CONTENT_STATUS_DONE) &&
        !nsjpeg_convert(jpeg_c)) {
        content_destroy(jpeg_c);
        return NSERROR_CLONE_FAILED;
    }
    *newc = jpeg_c;
    return NSERROR_OK;
}

static const content_handler nsjpeg_content_handler = {
    .create = nsjpeg_create,
    .data_complete = nsjpeg_convert,
    .destroy = image_cache_destroy,
    .redraw = image_cache_redraw,
    .clone = nsjpeg_clone,
    .get_internal = image_cache_get_internal,
    .type = image_cache_content_type,
    .is_opaque = image_cache_is_opaque,
    .no_share = false,
};

static const char *nsjpeg_types[] = {
    "image/jpeg",
    "image/jpg",
    "image/pjpeg",
};

CONTENT_FACTORY_REGISTER_TYPES(nsjpeg, nsjpeg_types, nsjpeg_content_handler);
