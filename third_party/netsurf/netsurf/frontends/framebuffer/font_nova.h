/*
 * NovaOS: TrueType font driver (stb_truetype), implemented in the NovaOS
 * tree as userland/netsurf/font_nova.c.
 *
 * This file is part of the NovaOS port of NetSurf.
 */

#ifndef NETSURF_FB_FONT_NOVA_H
#define NETSURF_FB_FONT_NOVA_H

#include <libnsfb.h>

/**
 * Plot a string with its baseline at y.
 */
nserror fb_nova_plot_text(nsfb_t *nsfb, const struct plot_font_style *fstyle,
		int x, int y, const char *text, size_t length);

#endif /* NETSURF_FB_FONT_NOVA_H */
