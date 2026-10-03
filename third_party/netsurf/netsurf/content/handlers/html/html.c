/*
 * Copyright 2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2010 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Implementation of HTML content handling.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <nsutils/time.h>

#include "utils/utils.h"
#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/http.h"
#include "utils/libdom.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/talloc.h"
#include "utils/utf8.h"
#include "utils/nsoption.h"
#include "utils/string.h"
#include "utils/ascii.h"
#include "netsurf/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/utf8.h"
#include "netsurf/keypress.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "content/hlcache.h"
#include "content/content_factory.h"
#include "content/textsearch.h"
#include "desktop/selection.h"
#include "desktop/scrollbar.h"
#include "desktop/textarea.h"
#include "netsurf/bitmap.h"
#include "javascript/js.h"
#include "desktop/gui_internal.h"

#include "css/select.h"
#include "html/html.h"
#include "html/private.h"
#include "html/dom_event.h"
#include "html/css.h"
#include "html/object.h"
#include "html/html_save.h"
#include "html/interaction.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/box_inspect.h"
#include "html/form_internal.h"
#include "html/imagemap.h"
#include "html/layout.h"
#include "html/textselection.h"
#ifdef _NOVAOS
#include "desktop/frames.h"
#endif

#define CHUNK 4096

/* Change these to 1 to cause a dump to stderr of the frameset or box
 * when the trees have been built.
 */
#define ALWAYS_DUMP_FRAMESET 0
#define ALWAYS_DUMP_BOX 0

static const char *html_types[] = {
	"application/xhtml+xml",
	"text/html"
};

/**
 * Fire an event at the DOM
 *
 * Helper that swallows DOM errors.
 *
 * \param[in] event   the event to fire at the DOM
 * \param[in] target  the event target
 * \return true on success
 */
static bool fire_dom_event(dom_event *event, dom_node *target)
{
	dom_exception exc;
	bool result;

	exc = dom_event_target_dispatch_event(target, event, &result);
	if (exc != DOM_NO_ERR) {
		return false;
	}

	return result;
}

/* Exported interface, see html_internal.h */
bool fire_generic_dom_event(dom_string *type, dom_node *target,
		bool bubbles, bool cancelable)
{
	dom_exception exc;
	dom_event *evt;
	bool result;

	exc = dom_event_create(&evt);
	if (exc != DOM_NO_ERR) return false;
	exc = dom_event_init(evt, type, bubbles, cancelable);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		return false;
	}
	NSLOG(netsurf, INFO, "Dispatching '%*s' against %p",
	      (int)dom_string_length(type), dom_string_data(type), target);
	result = fire_dom_event(evt, target);
	dom_event_unref(evt);
	return result;
}

/* Exported interface, see html_internal.h */
bool fire_dom_keyboard_event(dom_string *type, dom_node *target,
		bool bubbles, bool cancelable, uint32_t key)
{
	bool is_special = key <= 0x001F || (0x007F <= key && key <= 0x009F);
	dom_string *dom_key = NULL;
	dom_keyboard_event *evt;
	dom_exception exc;
	bool result;

	if (is_special) {
		switch (key) {
		case NS_KEY_ESCAPE:
			dom_key = dom_string_ref(corestring_dom_Escape);
			break;
		case NS_KEY_LEFT:
			dom_key = dom_string_ref(corestring_dom_ArrowLeft);
			break;
		case NS_KEY_RIGHT:
			dom_key = dom_string_ref(corestring_dom_ArrowRight);
			break;
		case NS_KEY_UP:
			dom_key = dom_string_ref(corestring_dom_ArrowUp);
			break;
		case NS_KEY_DOWN:
			dom_key = dom_string_ref(corestring_dom_ArrowDown);
			break;
		case NS_KEY_PAGE_UP:
			dom_key = dom_string_ref(corestring_dom_PageUp);
			break;
		case NS_KEY_PAGE_DOWN:
			dom_key = dom_string_ref(corestring_dom_PageDown);
			break;
		case NS_KEY_TEXT_START:
			dom_key = dom_string_ref(corestring_dom_Home);
			break;
		case NS_KEY_TEXT_END:
			dom_key = dom_string_ref(corestring_dom_End);
			break;
		default:
			dom_key = NULL;
			break;
		}
	} else {
		char utf8[6];
		size_t length = utf8_from_ucs4(key, utf8);
		utf8[length] = '\0';

		exc = dom_string_create((const uint8_t *)utf8, strlen(utf8),
				&dom_key);
		if (exc != DOM_NO_ERR) {
			return exc;
		}
	}

	exc = dom_keyboard_event_create(&evt);
	if (exc != DOM_NO_ERR) {
		dom_string_unref(dom_key);
		return false;
	}

	exc = dom_keyboard_event_init(evt, type, bubbles, cancelable, NULL,
			dom_key, NULL, DOM_KEY_LOCATION_STANDARD, false,
			false, false, false, false, false);
	dom_string_unref(dom_key);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		return false;
	}

	NSLOG(netsurf, INFO, "Dispatching '%*s' against %p",
			(int)dom_string_length(type), dom_string_data(type), target);

	result = fire_dom_event((dom_event *) evt, target);
	dom_event_unref(evt);
	return result;
}

/**
 * Perform post-box-creation conversion of a document
 *
 * \param c        HTML content to complete conversion of
 * \param success  Whether box tree construction was successful
 */
static void html_box_convert_done(html_content *c, bool success)
{
	nserror err;
	dom_exception exc; /* returned by libdom functions */
	dom_node *html;

	NSLOG(netsurf, INFO, "DOM to box conversion complete (content %p)", c);

	c->box_conversion_context = NULL;

	/* Clean up and report error if unsuccessful or aborted */
	if ((success == false) || (c->aborted)) {
		html_object_free_objects(c);

		if (success == false) {
			content_broadcast_error(&c->base, NSERROR_BOX_CONVERT, NULL);
		} else {
			content_broadcast_error(&c->base, NSERROR_STOPPED, NULL);
		}

		content_set_error(&c->base);
		return;
	}


#if ALWAYS_DUMP_BOX
	box_dump(stderr, c->layout->children, 0, true);
#endif
#if ALWAYS_DUMP_FRAMESET
	if (c->frameset)
		html_dump_frameset(c->frameset, 0);
#endif

	exc = dom_document_get_document_element(c->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		/** @todo should this call html_object_free_objects(c);
		 * like the other error paths
		 */
		NSLOG(netsurf, INFO, "error retrieving html element from dom");
		content_broadcast_error(&c->base, NSERROR_DOM, NULL);
		content_set_error(&c->base);
		return;
	}

	/* extract image maps - can't do this sensibly in dom_to_box */
	err = imagemap_extract(c);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, INFO, "imagemap extraction failed");
		html_object_free_objects(c);
		content_broadcast_error(&c->base, err, NULL);
		content_set_error(&c->base);
		dom_node_unref(html);
		return;
	}
	/*imagemap_dump(c);*/

	/* Destroy the parser binding */
	dom_hubbub_parser_destroy(c->parser);
	c->parser = NULL;

	content_set_ready(&c->base);

	html_proceed_to_done(c);

	dom_node_unref(html);
}

/* Documented in html_internal.h */
nserror
html_proceed_to_done(html_content *html)
{
	switch (content__get_status(&html->base)) {
	case CONTENT_STATUS_READY:
		if (html->base.active == 0) {
			content_set_done(&html->base);
			return NSERROR_OK;
		}
		break;
	case CONTENT_STATUS_DONE:
		/* fallthrough */
	case CONTENT_STATUS_LOADING:
		return NSERROR_OK;
	default:
		NSLOG(netsurf, ERROR, "Content status unexpectedly not LOADING/READY/DONE");
		break;
	}
	return NSERROR_UNKNOWN;
}


static void html_get_dimensions(html_content *htmlc)
{
	css_fixed device_dpi = nscss_screen_dpi;
	unsigned f_size;
	unsigned f_min;
	unsigned w;
	unsigned h;
	union content_msg_data msg_data = {
		.getdims = {
			.viewport_width = &w,
			.viewport_height = &h,
		},
	};

	content_broadcast(&htmlc->base, CONTENT_MSG_GETDIMS, &msg_data);


	w = css_unit_device2css_px(INTTOFIX(w), device_dpi);
	h = css_unit_device2css_px(INTTOFIX(h), device_dpi);

	htmlc->media.width  = w;
	htmlc->media.height = h;
	htmlc->unit_len_ctx.viewport_width  = w;
	htmlc->unit_len_ctx.viewport_height = h;
	htmlc->unit_len_ctx.device_dpi = device_dpi;

	/** \todo Change nsoption font sizes to px. */
	f_size = FDIV(FMUL(F_96, FDIV(INTTOFIX(nsoption_int(font_size)), F_10)), F_72);
	f_min  = FDIV(FMUL(F_96, FDIV(INTTOFIX(nsoption_int(font_min_size)), F_10)), F_72);

	htmlc->unit_len_ctx.font_size_default = f_size;
	htmlc->unit_len_ctx.font_size_minimum = f_min;
}

#ifdef _NOVAOS
/*
 * NovaOS: pages a script changes after layout.  Upstream NetSurf builds
 * the box tree once ("NS layout is static"), so a DOM change made by a
 * script after the page was laid out (a new element, new text, a changed
 * style or class attribute, a new stylesheet) never showed.  Here such a
 * change schedules html_relayout, which builds the boxes again from the
 * DOM, then reformats and redraws the page.  Changes made in one go by a
 * script coalesce into one rebuild.
 *
 * Only the part of the page a script changed is built again: the boxes
 * below the lowest block-level box above every change since the last
 * rebuild (html_schedule_relayout_node; a changed attribute counts as a
 * change to the element's parent, since sibling selectors can restyle
 * the element's later siblings).  A new stylesheet, or a change that
 * reaches the root element, builds the whole tree again; a change in the
 * head builds nothing.  Form controls keep their state (it lives in the
 * DOM and the form structures, not in the boxes), images carry on in the
 * new boxes, and iframes keep their browser windows (relinked to the new
 * boxes; opened again only when the page's iframes changed); the text
 * selection, a drag or the caret in a text field are dropped.  Frameset
 * pages keep upstream's static layout (they have no boxes to change).
 */

/** delay before a rebuild, so a burst of changes coalesces (ms) */
#define HTML_RELAYOUT_DELAY 20

/** A talloc context holding a subtree's boxes built again: freed with
 * the boxes when that part of the page is built again */
struct html_relayout_chunk {
	dom_node *root;		/**< node whose children the boxes are */
	void *ctx;		/**< child of the content's bctx */
	struct html_relayout_chunk *next;
};

static void html_relayout(void *p);

/** whether the content has a laid-out box tree a rebuild can replace */
static bool html_can_relayout(html_content *htmlc)
{
	content_status status = content__get_status(&htmlc->base);

	return htmlc->layout != NULL &&
		htmlc->box_conversion_context == NULL &&
		htmlc->had_initial_layout &&
		htmlc->aborted == false &&
		htmlc->frameset == NULL &&
		(status == CONTENT_STATUS_READY || status == CONTENT_STATUS_DONE);
}

/* exported function documented in html/private.h */
void html_schedule_relayout(html_content *htmlc)
{
	if (!html_can_relayout(htmlc))
		return;
	htmlc->relayout_full = true;
	guit->misc->schedule(HTML_RELAYOUT_DELAY, html_relayout, htmlc);
}

/** whether @a is @n or one of its ancestors */
static bool html_node_is_above(dom_node *a, dom_node *n)
{
	dom_node *cur = dom_node_ref(n);

	while (cur != NULL) {
		dom_node *parent = NULL;

		if (cur == a) {
			dom_node_unref(cur);
			return true;
		}
		dom_node_get_parent_node(cur, &parent);
		dom_node_unref(cur);
		cur = parent;
	}
	return false;
}

/** whether @n is the head element or inside it */
static bool html_node_in_head(dom_node *n)
{
	dom_node *cur = dom_node_ref(n);

	while (cur != NULL) {
		dom_node *parent = NULL;
		dom_node_type type;
		dom_html_element_type tag;

		if (dom_node_get_node_type(cur, &type) == DOM_NO_ERR &&
				type == DOM_ELEMENT_NODE &&
				dom_html_element_get_tag_type(cur, &tag) ==
					DOM_NO_ERR &&
				tag == DOM_HTML_ELEMENT_TYPE_HEAD) {
			dom_node_unref(cur);
			return true;
		}
		dom_node_get_parent_node(cur, &parent);
		dom_node_unref(cur);
		cur = parent;
	}
	return false;
}

/* exported function documented in html/private.h */
void html_schedule_relayout_node(html_content *htmlc, dom_node *node)
{
	dom_node *a;

	if (!html_can_relayout(htmlc) || node == NULL)
		return;
	if (html_node_in_head(node))
		return;		/* (a new stylesheet comes through
				 * html_finish_conversion) */

	if (htmlc->relayout_node == NULL) {
		htmlc->relayout_node = dom_node_ref(node);
	} else {
		/* the lowest node above both changes */
		a = dom_node_ref(htmlc->relayout_node);
		while (a != NULL && !html_node_is_above(a, node)) {
			dom_node *parent = NULL;
			dom_node_get_parent_node(a, &parent);
			dom_node_unref(a);
			a = parent;
		}
		dom_node_unref(htmlc->relayout_node);
		htmlc->relayout_node = a;
		if (a == NULL)
			htmlc->relayout_full = true;
	}
	guit->misc->schedule(HTML_RELAYOUT_DELAY, html_relayout, htmlc);
}

/** forget every node's box (the old tree's boxes are about to go) and
 * what libcss cached on it (its style is selected again) */
static void html_relayout_forget_boxes(dom_node *root)
{
	dom_node *n = dom_node_ref(root);

	while (n != NULL) {
		dom_node *next = NULL;
		void *old = NULL;
		dom_node_type type;

		if (dom_node_get_node_type(n, &type) == DOM_NO_ERR &&
				type == DOM_ELEMENT_NODE) {
			dom_node_set_user_data(n,
					corestring_dom___ns_key_box_node_data,
					NULL, NULL, &old);
			nscss_forget_node_data(n);
			dom_node_get_first_child(n, &next);
		}
		/* next in document order, never leaving root */
		while (next == NULL && n != NULL) {
			if (n == root) {
				dom_node_unref(n);
				n = NULL;
				break;
			}
			dom_node_get_next_sibling(n, &next);
			if (next == NULL) {
				dom_node *parent = NULL;
				dom_node_get_parent_node(n, &parent);
				dom_node_unref(n);
				n = parent;
			}
		}
		if (n != NULL) {
			dom_node_unref(n);
			n = next;
		}
	}
}

/** whether box @b is @within or below it */
static bool html_box_is_within(const struct box *b, const struct box *within)
{
	for (; b != NULL; b = b->parent)
		if (b == within)
			return true;
	return false;
}

/** detach the form controls from the old boxes (all, or those below
 * @within) */
static void html_relayout_forget_controls(html_content *htmlc,
		struct box *within)
{
	struct form *f;
	struct form_control *ctl;

	for (f = htmlc->forms; f != NULL; f = f->prev) {
		for (ctl = f->controls; ctl != NULL; ctl = ctl->next) {
			if (within != NULL && (ctl->box == NULL ||
					ctl->box == within ||
					!html_box_is_within(ctl->box, within)))
				continue;
			ctl->box = NULL;
			switch (ctl->type) {
			case GADGET_TEXTAREA:
			case GADGET_TEXTBOX:
			case GADGET_PASSWORD:
				/* the new box makes a new text area from
				 * the value in the DOM */
				if (ctl->data.text.ta != NULL) {
					textarea_destroy(ctl->data.text.ta);
					ctl->data.text.ta = NULL;
				}
				break;
			case GADGET_SELECT: {
				/* the new box adds the options again */
				struct form_option *o, *next;
				if (ctl->data.select.menu != NULL)
					form_free_select_menu(ctl);
				for (o = ctl->data.select.items; o != NULL; o = next) {
					next = o->next;
					free(o->text);
					free(o->value);
					free(o);
				}
				ctl->data.select.items = NULL;
				ctl->data.select.last_item = NULL;
				ctl->data.select.current = NULL;
				ctl->data.select.num_items = 0;
				ctl->data.select.num_selected = 0;
				break;
			}
			default:
				break;
			}
		}
	}
}

/* exported function documented in html/private.h */
void html_relayout_node_removed(html_content *htmlc, dom_node *node)
{
	if (html_can_relayout(htmlc))
		html_relayout_forget_boxes(node);
}

static bool html_relayout_ok;

static void html_relayout_done(html_content *htmlc, bool success)
{
	(void)htmlc;
	html_relayout_ok = success;
}

/** drop everything that points into the boxes about to go (all of them,
 * or those below @within) */
static void html_relayout_detach(html_content *htmlc, struct box *within)
{
	union html_drag_owner no_drag = { .no_owner = true };
	union html_focus_owner self_focus = { .self = true };
	union html_selection_owner no_sel = { .none = true };

	if (htmlc->drag_type != HTML_DRAG_NONE)
		html_set_drag_type(htmlc, HTML_DRAG_NONE, no_drag, NULL);
	if (htmlc->focus_type != HTML_FOCUS_SELF)
		html_set_focus(htmlc, HTML_FOCUS_SELF, self_focus, true,
				0, 0, 0, NULL);
	if (htmlc->selection_type != HTML_SELECTION_NONE)
		html_set_selection(htmlc, HTML_SELECTION_NONE, no_sel, true);
	selection_clear(htmlc->sel, false);
	html_relayout_forget_controls(htmlc, within);
	html_object_stash_box_objects(htmlc, within);
	imagemap_destroy(htmlc);
	if (htmlc->bw != NULL)
		browser_window_unlink_iframes(htmlc->bw, within);
}

/** after the rebuild: images not asked for again go, iframes find their
 * new boxes, image maps are read again */
static void html_relayout_attach(html_content *htmlc)
{
	html_object_drop_stash(htmlc);
	imagemap_extract(htmlc);
	if (htmlc->bw != NULL)
		browser_window_relink_iframes(htmlc->bw);
}

/** free a box and the boxes below it (they were allocated one by one in
 * the content's talloc context) */
static void html_relayout_free_boxes(struct box *b)
{
	while (b != NULL) {
		struct box *next = b->next;

		html_relayout_free_boxes(b->children);
		if (b->list_marker != NULL)
			html_relayout_free_boxes(b->list_marker);
		talloc_free(b);
		b = next;
	}
}

/** the node whose box's children a rebuild for a change at or below
 * @node replaces, or NULL when only a full rebuild will do */
static dom_node *html_relayout_root(html_content *htmlc, dom_node *node)
{
	dom_node *n = dom_node_ref(node);

	while (n != NULL) {
		dom_node *parent = NULL;
		dom_node_type type;

		if (dom_node_get_node_type(n, &type) != DOM_NO_ERR ||
				type == DOM_DOCUMENT_NODE)
			break;
		if (type == DOM_ELEMENT_NODE) {
			struct box *box = box_for_node(n);

			dom_node_get_parent_node(n, &parent);
			if (parent != NULL) {
				dom_node_type ptype = DOM_ELEMENT_NODE;
				dom_node_get_node_type(parent, &ptype);
				if (ptype == DOM_DOCUMENT_NODE) {
					/* the root element: everything */
					dom_node_unref(parent);
					break;
				}
			}
			if (box != NULL &&
					(box->type == BOX_BLOCK ||
					 box->type == BOX_INLINE_BLOCK ||
					 box->type == BOX_TABLE_CELL) &&
					(box->flags & CONVERT_CHILDREN) &&
					!(box->flags & IS_REPLACED) &&
					box->gadget == NULL &&
					box->object == NULL &&
					box->iframe == NULL &&
					box->styles != NULL &&
					box != htmlc->layout) {
				if (parent != NULL)
					dom_node_unref(parent);
				return n;
			}
		} else {
			dom_node_get_parent_node(n, &parent);
		}
		dom_node_unref(n);
		n = parent;
	}
	if (n != NULL)
		dom_node_unref(n);
	return NULL;
}

/** build the boxes below @root again (its own box stays) */
static bool html_relayout_subtree(html_content *htmlc, dom_node *root)
{
	struct html_relayout_chunk *chunk, **link, *dead = NULL;
	struct box *box = box_for_node(root);
	struct box *old = NULL;
	int *bctx = htmlc->bctx;
	nserror err;

	chunk = calloc(1, sizeof(*chunk));
	if (chunk == NULL)
		return false;
	chunk->ctx = talloc_zero(bctx, int);
	if (chunk->ctx == NULL) {
		free(chunk);
		return false;
	}
	chunk->root = dom_node_ref(root);

	html_relayout_detach(htmlc, box);
	{
		/* iframes of the old boxes leave the content's list */
		struct content_html_iframe **ifl = &htmlc->iframe;
		while (*ifl != NULL) {
			struct content_html_iframe *f = *ifl;
			if (f->box != box &&
					html_box_is_within(f->box, box)) {
				*ifl = f->next;
				talloc_free(f);
			} else {
				ifl = &f->next;
			}
		}
	}
	/* the descendants' boxes and styles go (the root keeps its own) */
	{
		dom_node *child = NULL, *next;
		dom_node_get_first_child(root, &child);
		while (child != NULL) {
			html_relayout_forget_boxes(child);
			dom_node_get_next_sibling(child, &next);
			dom_node_unref(child);
			child = next;
		}
	}

	/* new boxes go in the chunk */
	htmlc->bctx = chunk->ctx;
	err = dom_to_box_subtree(root, htmlc, &old);
	htmlc->bctx = bctx;
	html_relayout_attach(htmlc);

	/* the old boxes go, then the chunks they were built in */
	html_relayout_free_boxes(old);
	for (link = &htmlc->relayout_chunks; *link != NULL; ) {
		struct html_relayout_chunk *c = *link;
		if (html_node_is_above(root, c->root)) {
			*link = c->next;
			c->next = dead;
			dead = c;
		} else {
			link = &c->next;
		}
	}
	while (dead != NULL) {
		struct html_relayout_chunk *c = dead;
		dead = c->next;
		talloc_free(c->ctx);
		dom_node_unref(c->root);
		free(c);
	}
	chunk->next = htmlc->relayout_chunks;
	htmlc->relayout_chunks = chunk;

	/* the layout starts from this box (layout_document) */
	htmlc->layout_target = box;

	/* the widths of the box and the boxes above it are measured again */
	for (; box != NULL; box = box->parent)
		box->max_width = UNKNOWN_MAX_WIDTH;

	return err == NSERROR_OK;
}

/** forget the subtree chunks (the content's bctx owns their memory) */
static void html_relayout_drop_chunks(html_content *htmlc)
{
	while (htmlc->relayout_chunks != NULL) {
		struct html_relayout_chunk *c = htmlc->relayout_chunks;
		htmlc->relayout_chunks = c->next;
		dom_node_unref(c->root);
		free(c);
	}
}

/** rebuild the whole box tree from the DOM */
static bool html_relayout_all(html_content *htmlc, dom_node *html)
{
	css_select_ctx *select_ctx = NULL;
	int *old_bctx;
	nserror err;

	/* a new selection context: stylesheets a script added count now */
	err = html_css_new_selection_context(htmlc, &select_ctx);
	if (err != NSERROR_OK)
		return true;	/* (keep the old tree) */

	html_relayout_detach(htmlc, NULL);
	html_relayout_forget_boxes(html);

	if (htmlc->select_ctx != NULL)
		css_select_ctx_destroy(htmlc->select_ctx);
	htmlc->select_ctx = select_ctx;

	/* build the new tree in a talloc context of its own; the old
	 * tree's iframe list goes with the old context */
	old_bctx = htmlc->bctx;
	htmlc->bctx = NULL;
	htmlc->layout = NULL;
	htmlc->iframe = NULL;
	html_relayout_drop_chunks(htmlc);
	html_relayout_ok = false;
	err = dom_to_box_sync(html, htmlc, html_relayout_done);
	html_relayout_attach(htmlc);
	talloc_free(old_bctx);
	return err == NSERROR_OK && html_relayout_ok && htmlc->layout != NULL;
}


/*
 * Layout timings and checks (environment variables, for the nstest
 * self-test and for measuring):
 *   NETSURF_LAYOUT_LOG=file   append a line per script-driven relayout
 *   NETSURF_LAYOUT_CHECK=1    after every third layout from a changed box
 *                             (so that the two before it start from what
 *                             such layouts left), lay out the whole page
 *                             too and compare every box
 *   NETSURF_LAYOUT=full       always lay out the whole page
 */
static int html_layout_env = -1;
static FILE *html_layout_log;
static bool html_layout_check, html_layout_full;
static unsigned html_layout_unchecked;	/* layouts since the last check */

static void html_layout_env_read(void)
{
	const char *v;

	html_layout_env = 1;
	v = getenv("NETSURF_LAYOUT_LOG");
	if (v != NULL && *v != '\0')
		html_layout_log = fopen(v, "a");
	v = getenv("NETSURF_LAYOUT_CHECK");
	html_layout_check = v != NULL && *v == '1';
	v = getenv("NETSURF_LAYOUT");
	html_layout_full = v != NULL && strcmp(v, "full") == 0;
}

/** a box's geometry, for comparing two layouts */
struct html_layout_shot {
	struct box *box;
	int type, x, y, width, height;
	int d[4];
	size_t length;
};

static void html_layout_shoot(struct box *b, struct html_layout_shot **shots,
		size_t *n, size_t *alloc)
{
	for (; b != NULL; b = b->next) {
		struct html_layout_shot *s;
		if (*n == *alloc) {
			size_t a = *alloc ? *alloc * 2 : 1024;
			s = realloc(*shots, a * sizeof **shots);
			if (s == NULL)
				return;
			*shots = s;
			*alloc = a;
		}
		s = &(*shots)[(*n)++];
		s->box = b;
		s->type = b->type;
		s->x = b->x;
		s->y = b->y;
		s->width = b->width;
		s->height = b->height;
		s->d[0] = b->descendant_x0;
		s->d[1] = b->descendant_y0;
		s->d[2] = b->descendant_x1;
		s->d[3] = b->descendant_y1;
		s->length = b->length;
		if (b->list_marker != NULL)
			html_layout_shoot(b->list_marker, shots, n, alloc);
		html_layout_shoot(b->children, shots, n, alloc);
		if (b->type == BOX_INLINE_END)
			;	/* (siblings: visited by the loop) */
	}
}

/** compare the layout from a changed box with a full one; the number of
 * boxes that differ (the first described in @first) */
static size_t html_layout_compare(html_content *htmlc, size_t *boxes,
		char *first, size_t first_size, uint64_t *us_full)
{
	struct html_layout_shot *a = NULL, *b = NULL;
	size_t na = 0, nb = 0, aa = 0, ab = 0, i, bad = 0;
	int w0 = htmlc->base.width, h0 = htmlc->base.height;
	uint64_t t0;

	html_layout_shoot(htmlc->layout, &a, &na, &aa);
	t0 = layout_clock_us();
	content__reformat(&htmlc->base, false, htmlc->base.available_width,
			htmlc->base.available_height);
	*us_full = layout_clock_us() - t0;
	html_layout_shoot(htmlc->layout, &b, &nb, &ab);
	*boxes = nb;
	first[0] = '\0';
	if (w0 != htmlc->base.width || h0 != htmlc->base.height) {
		bad++;
		snprintf(first, first_size, "page %ix%i, full %ix%i",
				w0, h0, htmlc->base.width, htmlc->base.height);
	}
	if (na != nb) {
		bad++;
		if (first[0] == '\0')
			snprintf(first, first_size, "%u boxes, full %u",
					(unsigned) na, (unsigned) nb);
	}
	for (i = 0; i < na && i < nb; i++) {
		struct html_layout_shot *p = &a[i], *q = &b[i];
		if (p->box == q->box && p->type == q->type &&
				p->x == q->x && p->y == q->y &&
				p->width == q->width && p->height == q->height &&
				memcmp(p->d, q->d, sizeof p->d) == 0 &&
				p->length == q->length)
			continue;
		if (bad++ == 0 || first[0] == '\0') {
			dom_string *name = NULL;
			if (q->box->node != NULL)
				dom_node_get_node_name(q->box->node, &name);
			snprintf(first, first_size, "box %u (type %i <%s>): "
					"%i,%i %ix%i [%i %i %i %i], full %i,%i "
					"%ix%i [%i %i %i %i]", (unsigned) i, q->type,
					name ? dom_string_data(name) : "",
					p->x, p->y, p->width, p->height,
					p->d[0], p->d[1], p->d[2], p->d[3],
					q->x, q->y, q->width, q->height,
					q->d[0], q->d[1], q->d[2], q->d[3]);
			if (name != NULL)
				dom_string_unref(name);
		}
	}
	free(a);
	free(b);
	return bad;
}

/** rebuild the changed part of the box tree, reformat and redraw */
static void html_relayout(void *p)
{
	html_content *htmlc = p;
	dom_node *html = NULL, *root = NULL;
	bool full, ok;
	uint64_t t0 = 0, t1 = 0, t2 = 0;

	if (!html_can_relayout(htmlc) || htmlc->base.locked)
		return;
	if (htmlc->visible_select_menu != NULL || htmlc->reflowing) {
		/* a select menu is open (it points into the boxes): later */
		guit->misc->schedule(100, html_relayout, htmlc);
		return;
	}
	if (dom_document_get_document_element(htmlc->document,
			(void *) &html) != DOM_NO_ERR || html == NULL)
		return;

	full = htmlc->relayout_full;
	if (!full && htmlc->relayout_node != NULL) {
		root = html_relayout_root(htmlc, htmlc->relayout_node);
		full = (root == NULL);
	}
	htmlc->relayout_full = false;
	if (htmlc->relayout_node != NULL) {
		dom_node_unref(htmlc->relayout_node);
		htmlc->relayout_node = NULL;
	}
	if (!full && root == NULL) {
		dom_node_unref(html);
		return;
	}

	if (html_layout_env < 0)
		html_layout_env_read();
	t0 = layout_clock_us();
	if (full)
		ok = html_relayout_all(htmlc, html);
	else
		ok = html_relayout_subtree(htmlc, root);
	if (root != NULL)
		dom_node_unref(root);
	dom_node_unref(html);
	if (!ok || htmlc->layout == NULL) {
		htmlc->layout_target = NULL;
		NSLOG(netsurf, ERROR, "box tree rebuild failed");
		content_broadcast_error(&htmlc->base, NSERROR_BOX_CONVERT, NULL);
		content_set_error(&htmlc->base);
		return;
	}
	t1 = layout_clock_us();

	/* lay out at the size the page had (from the rebuilt box when the
	 * rest of the page kept its boxes) */
	if (full || html_layout_full)
		htmlc->layout_target = NULL;
	content__reformat(&htmlc->base, false, htmlc->base.available_width,
			htmlc->base.available_height);
	htmlc->layout_target = NULL;
	t2 = layout_clock_us();

	if (html_layout_log != NULL) {
		struct html_layout_record *r = &htmlc->layout_rec;
		fprintf(html_layout_log, "relayout: %s rebuild %u us, layout "
				"%u us (%s; widths %u us, flow %u us, "
				"placing %u us; %u boxes laid out, %u moved "
				"whole)%s%s\n",
				full ? "full" : "subtree",
				(unsigned)(t1 - t0), (unsigned)(t2 - t1),
				r->incremental ? "from the changed box" :
						"whole page",
				(unsigned) r->us_minmax, (unsigned) r->us_flow,
				(unsigned) r->us_place, r->laid, r->kept,
				r->full_why && !full ? ": " : "",
				r->full_why && !full ? r->full_why : "");
		if (html_layout_check && htmlc->layout_rec.incremental &&
				++html_layout_unchecked == 3) {
			char first[256];
			size_t boxes = 0, bad;
			uint64_t us_full = 0;
			html_layout_unchecked = 0;
			bad = html_layout_compare(htmlc, &boxes, first,
					sizeof first, &us_full);
			fprintf(html_layout_log, "check: whole page %u us "
					"(widths %u us, flow %u us, placing %u "
					"us), %u boxes, %u differ%s%s\n",
					(unsigned) us_full,
					(unsigned) r->us_minmax,
					(unsigned) r->us_flow,
					(unsigned) r->us_place, (unsigned) boxes,
					(unsigned) bad, bad ? ": " : "",
					bad ? first : "");
		}
		fflush(html_layout_log);
	}

	html_proceed_to_done(htmlc);
}

/** stop a scheduled rebuild (the content goes away) */
static void html_cancel_relayout(html_content *htmlc)
{
	guit->misc->schedule(-1, html_relayout, htmlc);
	if (htmlc->relayout_node != NULL) {
		dom_node_unref(htmlc->relayout_node);
		htmlc->relayout_node = NULL;
	}
	html_relayout_drop_chunks(htmlc);
}
#endif


/* exported function documented in html/html_internal.h */
void html_finish_conversion(html_content *htmlc)
{
	union content_msg_data msg_data;
	dom_exception exc; /* returned by libdom functions */
	dom_node *html;
	nserror error;

	/* Bail out if we've been aborted */
	if (htmlc->aborted) {
		content_broadcast_error(&htmlc->base, NSERROR_STOPPED, NULL);
		content_set_error(&htmlc->base);
		return;
	}

	/* If we already have a selection context, then we have already
	 * "finished" conversion.  We can get here twice if e.g. some JS
	 * adds a new stylesheet, and the stylesheet gets added after
	 * the HTML content is initially finished.
	 *
	 * If we didn't do this, the HTML content would try to rebuild the
	 * box tree for the html content when this new stylesheet is ready.
	 * NetSurf has no concept of dynamically changing documents, so this
	 * would break badly.
	 */
	if (htmlc->select_ctx != NULL) {
#ifdef _NOVAOS
		/* NovaOS: lay the page out again with the new stylesheet */
		html_schedule_relayout(htmlc);
#else
		NSLOG(netsurf, INFO,
				"Ignoring style change: NS layout is static.");
#endif
		return;
	}

	/* create new css selection context */
	error = html_css_new_selection_context(htmlc, &htmlc->select_ctx);
	if (error != NSERROR_OK) {
		content_broadcast_error(&htmlc->base, error, NULL);
		content_set_error(&htmlc->base);
		return;
	}


	/* fire a simple event named load at the Document's Window
	 * object, but with its target set to the Document object (and
	 * the currentTarget set to the Window object)
	 */
	if (htmlc->jsthread != NULL) {
		js_fire_event(htmlc->jsthread, "load", htmlc->document, NULL);
	}

	/* convert dom tree to box tree */
	NSLOG(netsurf, INFO, "DOM to box (%p)", htmlc);
	content_set_status(&htmlc->base, messages_get("Processing"));
	msg_data.explicit_status_text = NULL;
	content_broadcast(&htmlc->base, CONTENT_MSG_STATUS, &msg_data);

	exc = dom_document_get_document_element(htmlc->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		NSLOG(netsurf, INFO, "error retrieving html element from dom");
		content_broadcast_error(&htmlc->base, NSERROR_DOM, NULL);
		content_set_error(&htmlc->base);
		return;
	}

	html_get_dimensions(htmlc);

	error = dom_to_box(html, htmlc, html_box_convert_done, &htmlc->box_conversion_context);
	if (error != NSERROR_OK) {
		NSLOG(netsurf, INFO, "box conversion failed");
		dom_node_unref(html);
		html_object_free_objects(htmlc);
		content_broadcast_error(&htmlc->base, error, NULL);
		content_set_error(&htmlc->base);
		return;
	}

	dom_node_unref(html);
}


static void
html_document_user_data_handler(dom_node_operation operation,
				dom_string *key, void *data,
				struct dom_node *src,
				struct dom_node *dst)
{
	if (dom_string_isequal(corestring_dom___ns_key_html_content_data,
			       key) == false || data == NULL) {
		return;
	}

	switch (operation) {
	case DOM_NODE_CLONED:
		NSLOG(netsurf, INFO, "Cloned");
		break;
	case DOM_NODE_RENAMED:
		NSLOG(netsurf, INFO, "Renamed");
		break;
	case DOM_NODE_IMPORTED:
		NSLOG(netsurf, INFO, "imported");
		break;
	case DOM_NODE_ADOPTED:
		NSLOG(netsurf, INFO, "Adopted");
		break;
	case DOM_NODE_DELETED:
		/* This is the only path I expect */
		break;
	default:
		NSLOG(netsurf, INFO, "User data operation not handled.");
		assert(0);
	}
}


static nserror
html_create_html_data(html_content *c, const http_parameter *params)
{
	lwc_string *charset;
	nserror nerror;
	dom_hubbub_parser_params parse_params;
	dom_hubbub_error error;
	dom_exception err;
	void *old_node_data;
	const char *prefer_color_mode = (nsoption_bool(prefer_dark_mode)) ?
			"dark" : "light";

	c->parser = NULL;
	c->parse_completed = false;
	c->conversion_begun = false;
	c->document = NULL;
	c->quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
	c->encoding = NULL;
	c->base_url = nsurl_ref(content_get_url(&c->base));
	c->base_target = NULL;
	c->aborted = false;
	c->refresh = false;
	c->reflowing = false;
	c->title = NULL;
	c->bctx = NULL;
	c->layout = NULL;
	c->background_colour = NS_TRANSPARENT;
	c->stylesheet_count = 0;
	c->stylesheets = NULL;
	c->select_ctx = NULL;
	c->media.type = CSS_MEDIA_SCREEN;
	c->universal = NULL;
	c->num_objects = 0;
	c->object_list = NULL;
	c->forms = NULL;
	c->imagemaps = NULL;
	c->bw = NULL;
	c->frameset = NULL;
	c->iframe = NULL;
	c->page = NULL;
	c->font_func = guit->layout;
	c->drag_type = HTML_DRAG_NONE;
	c->drag_owner.no_owner = true;
	c->selection_type = HTML_SELECTION_NONE;
	c->selection_owner.none = true;
	c->focus_type = HTML_FOCUS_SELF;
	c->focus_owner.self = true;
	c->scripts_count = 0;
	c->scripts = NULL;
	c->jsthread = NULL;

	c->enable_scripting = nsoption_bool(enable_javascript);
	c->base.active = 1; /* The html content itself is active */

	if (lwc_intern_string("*", SLEN("*"), &c->universal) != lwc_error_ok) {
		return NSERROR_NOMEM;
	}

	if (lwc_intern_string(prefer_color_mode, strlen(prefer_color_mode),
			&c->media.prefers_color_scheme) != lwc_error_ok) {
		lwc_string_unref(c->universal);
		c->universal = NULL;
		return NSERROR_NOMEM;
	}

	c->sel = selection_create((struct content *)c);

	nerror = http_parameter_list_find_item(params, corestring_lwc_charset, &charset);
	if (nerror == NSERROR_OK) {
		c->encoding = strdup(lwc_string_data(charset));

		lwc_string_unref(charset);

		if (c->encoding == NULL) {
			lwc_string_unref(c->universal);
			c->universal = NULL;
			lwc_string_unref(c->media.prefers_color_scheme);
			c->media.prefers_color_scheme = NULL;
			return NSERROR_NOMEM;

		}
		c->encoding_source = DOM_HUBBUB_ENCODING_SOURCE_HEADER;
	}

	/* Create the parser binding */
	parse_params.enc = c->encoding;
	parse_params.fix_enc = true;
	parse_params.enable_script = c->enable_scripting;
	parse_params.msg = NULL;
	parse_params.script = html_process_script;
	parse_params.ctx = c;
	parse_params.daf = html_dom_event_fetcher;

	error = dom_hubbub_parser_create(&parse_params,
					 &c->parser,
					 &c->document);
	if ((error != DOM_HUBBUB_OK) && (c->encoding != NULL)) {
		/* Ok, we don't support the declared encoding. Bailing out
		 * isn't exactly user-friendly, so fall back to autodetect */
		free(c->encoding);
		c->encoding = NULL;

		parse_params.enc = c->encoding;

		error = dom_hubbub_parser_create(&parse_params,
						 &c->parser,
						 &c->document);
	}
	if (error != DOM_HUBBUB_OK) {
		nsurl_unref(c->base_url);
		c->base_url = NULL;

		lwc_string_unref(c->universal);
		c->universal = NULL;
		lwc_string_unref(c->media.prefers_color_scheme);
		c->media.prefers_color_scheme = NULL;

		return libdom_hubbub_error_to_nserror(error);
	}

	err = dom_node_set_user_data(c->document,
				     corestring_dom___ns_key_html_content_data,
				     c, html_document_user_data_handler,
				     (void *) &old_node_data);
	if (err != DOM_NO_ERR) {
		dom_hubbub_parser_destroy(c->parser);
		c->parser = NULL;
		nsurl_unref(c->base_url);
		c->base_url = NULL;

		lwc_string_unref(c->universal);
		c->universal = NULL;
		lwc_string_unref(c->media.prefers_color_scheme);
		c->media.prefers_color_scheme = NULL;

		NSLOG(netsurf, INFO, "Unable to set user data.");
		return NSERROR_DOM;
	}

	assert(old_node_data == NULL);

	return NSERROR_OK;

}

/**
 * Create a CONTENT_HTML.
 *
 * The content_html_data structure is initialized and the HTML parser is
 * created.
 */

static nserror
html_create(const content_handler *handler,
	    lwc_string *imime_type,
	    const http_parameter *params,
	    llcache_handle *llcache,
	    const char *fallback_charset,
	    bool quirks,
	    struct content **c)
{
	html_content *html;
	nserror error;

	html = calloc(1, sizeof(html_content));
	if (html == NULL)
		return NSERROR_NOMEM;

	error = content__init(&html->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(html);
		return error;
	}

	error = html_create_html_data(html, params);
	if (error != NSERROR_OK) {
		content_broadcast_error(&html->base, error, NULL);
		free(html);
		return error;
	}

	error = html_css_new_stylesheets(html);
	if (error != NSERROR_OK) {
		content_broadcast_error(&html->base, error, NULL);
		free(html);
		return error;
	}

	*c = (struct content *) html;

	return NSERROR_OK;
}



static nserror
html_process_encoding_change(struct content *c,
			     const char *data,
			     unsigned int size)
{
	html_content *html = (html_content *) c;
	dom_hubbub_parser_params parse_params;
	dom_hubbub_error error;
	const char *encoding;
	const uint8_t *source_data;
	size_t source_size;

	/* Retrieve new encoding */
	encoding = dom_hubbub_parser_get_encoding(html->parser,
						  &html->encoding_source);
	if (encoding == NULL) {
		return NSERROR_NOMEM;
	}

	if (html->encoding != NULL) {
		free(html->encoding);
		html->encoding = NULL;
	}

	html->encoding = strdup(encoding);
	if (html->encoding == NULL) {
		return NSERROR_NOMEM;
	}

	/* Destroy binding */
	dom_hubbub_parser_destroy(html->parser);
	html->parser = NULL;

	if (html->document != NULL) {
		dom_node_unref(html->document);
	}

	parse_params.enc = html->encoding;
	parse_params.fix_enc = true;
	parse_params.enable_script = html->enable_scripting;
	parse_params.msg = NULL;
	parse_params.script = html_process_script;
	parse_params.ctx = html;
	parse_params.daf = html_dom_event_fetcher;

	/* Create new binding, using the new encoding */
	error = dom_hubbub_parser_create(&parse_params,
					 &html->parser,
					 &html->document);
	if (error != DOM_HUBBUB_OK) {
		/* Ok, we don't support the declared encoding. Bailing out
		 * isn't exactly user-friendly, so fall back to Windows-1252 */
		free(html->encoding);
		html->encoding = strdup("Windows-1252");
		if (html->encoding == NULL) {
			return NSERROR_NOMEM;
		}
		parse_params.enc = html->encoding;

		error = dom_hubbub_parser_create(&parse_params,
						 &html->parser,
						 &html->document);

		if (error != DOM_HUBBUB_OK) {
			return libdom_hubbub_error_to_nserror(error);
		}

	}

	source_data = content__get_source_data(c, &source_size);

	/* Reprocess all the data.  This is safe because
	 * the encoding is now specified at parser start which means
	 * it cannot be changed again.
	 */
	error = dom_hubbub_parser_parse_chunk(html->parser,
					      source_data,
					      source_size);

	return libdom_hubbub_error_to_nserror(error);
}


/**
 * Process data for CONTENT_HTML.
 */

static bool
html_process_data(struct content *c, const char *data, unsigned int size)
{
	html_content *html = (html_content *) c;
	dom_hubbub_error dom_ret;
	nserror err = NSERROR_OK; /* assume its all going to be ok */

	dom_ret = dom_hubbub_parser_parse_chunk(html->parser,
					      (const uint8_t *) data,
					      size);

	err = libdom_hubbub_error_to_nserror(dom_ret);

	/* deal with encoding change */
	if (err == NSERROR_ENCODING_CHANGE) {
		 err = html_process_encoding_change(c, data, size);
	}

	/* broadcast the error if necessary */
	if (err != NSERROR_OK) {
		content_broadcast_error(c, err, NULL);
		return false;
	}

	return true;
}


/**
 * Convert a CONTENT_HTML for display.
 *
 * The following steps are carried out in order:
 *
 *  - parsing to an XML tree is completed
 *  - stylesheets are fetched
 *  - the XML tree is converted to a box tree and object fetches are started
 *
 * On exit, the content status will be either CONTENT_STATUS_DONE if the
 * document is completely loaded or CONTENT_STATUS_READY if objects are still
 * being fetched.
 */

static bool html_convert(struct content *c)
{
	html_content *htmlc = (html_content *) c;
	dom_exception exc; /* returned by libdom functions */

	/* The quirk check and associated stylesheet fetch is "safe"
	 * once the root node has been inserted into the document
	 * which must have happened by this point in the parse.
	 *
	 * faliure to retrive the quirk mode or to start the
	 * stylesheet fetch is non fatal as this "only" affects the
	 * render and it would annoy the user to fail the entire
	 * render for want of a quirks stylesheet.
	 */
	exc = dom_document_get_quirks_mode(htmlc->document, &htmlc->quirks);
	if (exc == DOM_NO_ERR) {
		html_css_quirks_stylesheets(htmlc);
		NSLOG(netsurf, INFO, "quirks set to %d", htmlc->quirks);
	}

	htmlc->base.active--; /* the html fetch is no longer active */
	NSLOG(netsurf, INFO, "%d fetches active (%p)", htmlc->base.active, c);

	/* The parse cannot be completed here because it may be paused
	 * untill all the resources being fetched have completed.
	 */

	/* if there are no active fetches in progress no scripts are
	 * being fetched or they completed already.
	 */
	if (html_can_begin_conversion(htmlc)) {
		return html_begin_conversion(htmlc);
	}
	return true;
}

/* Exported interface documented in html_internal.h */
bool html_can_begin_conversion(html_content *htmlc)
{
	unsigned int i;

	/* Cannot begin conversion if we're still fetching stuff */
	if (htmlc->base.active != 0)
		return false;

	for (i = 0; i != htmlc->stylesheet_count; i++) {
		/* Cannot begin conversion if the stylesheets are modified */
		if (htmlc->stylesheets[i].modified)
			return false;
	}

	/* All is good, begin */
	return true;
}

bool
html_begin_conversion(html_content *htmlc)
{
	dom_node *html;
	nserror ns_error;
	struct form *f;
	dom_exception exc; /* returned by libdom functions */
	dom_string *node_name = NULL;
	dom_hubbub_error error;

	/* The act of completing the parse can result in additional data
	 * being flushed through the parser. This may result in new style or
	 * script nodes, upon which the conversion depends. Thus, once we
	 * have completed the parse, we must check again to see if we can
	 * begin the conversion. If we can't, we must stop and wait for the
	 * new styles/scripts to be processed. Once they have been processed,
	 * we will be called again to begin the conversion for real. Thus,
	 * we must also ensure that we don't attempt to complete the parse
	 * multiple times, so store a flag to indicate that parsing is
	 * complete to avoid repeating the completion pointlessly.
	 */
	if (htmlc->parse_completed == false) {
		NSLOG(netsurf, INFO, "Completing parse (%p)", htmlc);
		/* complete parsing */
		error = dom_hubbub_parser_completed(htmlc->parser);
		if (error == DOM_HUBBUB_HUBBUB_ERR_PAUSED && htmlc->base.active > 0) {
			/* The act of completing the parse failed because we've
			 * encountered a sync script which needs to run
			 */
			NSLOG(netsurf, INFO, "Completing parse brought synchronous JS to light, cannot complete yet");
			return true;
		}
		if (error != DOM_HUBBUB_OK) {
			NSLOG(netsurf, INFO, "Parsing failed");

			content_broadcast_error(&htmlc->base,
						libdom_hubbub_error_to_nserror(error),
						NULL);

			return false;
		}
		htmlc->parse_completed = true;
	}

	if (html_can_begin_conversion(htmlc) == false) {
		NSLOG(netsurf, INFO, "Can't begin conversion (%p)", htmlc);
		/* We can't proceed (see commentary above) */
		return true;
	}

	/* Give up processing if we've been aborted */
	if (htmlc->aborted) {
		NSLOG(netsurf, INFO, "Conversion aborted (%p) (active: %u)",
		      htmlc, htmlc->base.active);
		content_set_error(&htmlc->base);
		content_broadcast_error(&htmlc->base, NSERROR_STOPPED, NULL);
		return false;
	}

	/* Conversion begins proper at this point */
	htmlc->conversion_begun = true;

	/* complete script execution, including deferred scripts */
	html_script_exec(htmlc, true);

	/* fire a simple event that bubbles named DOMContentLoaded at
	 * the Document.
	 */

	/* get encoding */
	if (htmlc->encoding == NULL) {
		const char *encoding;

		encoding = dom_hubbub_parser_get_encoding(htmlc->parser,
					&htmlc->encoding_source);
		if (encoding == NULL) {
			content_broadcast_error(&htmlc->base,
						NSERROR_NOMEM,
						NULL);
			return false;
		}

		htmlc->encoding = strdup(encoding);
		if (htmlc->encoding == NULL) {
			content_broadcast_error(&htmlc->base,
						NSERROR_NOMEM,
						NULL);
			return false;
		}
	}

	/* locate root element and ensure it is html */
	exc = dom_document_get_document_element(htmlc->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		NSLOG(netsurf, INFO, "error retrieving html element from dom");
		content_broadcast_error(&htmlc->base, NSERROR_DOM, NULL);
		return false;
	}

	exc = dom_node_get_node_name(html, &node_name);
	if ((exc != DOM_NO_ERR) ||
	    (node_name == NULL) ||
	    (!dom_string_caseless_lwc_isequal(node_name,
			corestring_lwc_html))) {
		NSLOG(netsurf, INFO, "root element not html");
		content_broadcast_error(&htmlc->base, NSERROR_DOM, NULL);
		dom_node_unref(html);
		return false;
	}
	dom_string_unref(node_name);

	/* Retrieve forms from parser */
	htmlc->forms = html_forms_get_forms(htmlc->encoding,
			(dom_html_document *) htmlc->document);
	for (f = htmlc->forms; f != NULL; f = f->prev) {
		nsurl *action;

		/* Make all actions absolute */
		if (f->action == NULL || f->action[0] == '\0') {
			/* HTML5 4.10.22.3 step 9 */
			nsurl *doc_addr = content_get_url(&htmlc->base);
			ns_error = nsurl_join(htmlc->base_url,
					      nsurl_access(doc_addr),
					      &action);
		} else {
			ns_error = nsurl_join(htmlc->base_url,
					      f->action,
					      &action);
		}

		if (ns_error != NSERROR_OK) {
			content_broadcast_error(&htmlc->base, ns_error, NULL);

			dom_node_unref(html);
			return false;
		}

		free(f->action);
		f->action = strdup(nsurl_access(action));
		nsurl_unref(action);
		if (f->action == NULL) {
			content_broadcast_error(&htmlc->base,
						NSERROR_NOMEM,
						NULL);

			dom_node_unref(html);
			return false;
		}

		/* Ensure each form has a document encoding */
		if (f->document_charset == NULL) {
			f->document_charset = strdup(htmlc->encoding);
			if (f->document_charset == NULL) {
				content_broadcast_error(&htmlc->base,
							NSERROR_NOMEM,
							NULL);
				dom_node_unref(html);
				return false;
			}
		}
	}

	dom_node_unref(html);

	if (htmlc->base.active == 0) {
		html_finish_conversion(htmlc);
	}

	return true;
}


/**
 * Stop loading a CONTENT_HTML.
 *
 * called when the content is aborted. This must clean up any state
 * created during the fetch.
 */

static void html_stop(struct content *c)
{
	html_content *htmlc = (html_content *) c;

	switch (c->status) {
	case CONTENT_STATUS_LOADING:
		/* Still loading; simply flag that we've been aborted
		 * html_convert/html_finish_conversion will do the rest */
		htmlc->aborted = true;
		if (htmlc->jsthread != NULL) {
			/* Close the JS thread to cancel out any callbacks */
			js_closethread(htmlc->jsthread);
		}
		break;

	case CONTENT_STATUS_READY:
		html_object_abort_objects(htmlc);

		/* If there are no further active fetches and we're still
		 * in the READY state, transition to the DONE state. */
		if (c->status == CONTENT_STATUS_READY && c->active == 0) {
			content_set_done(c);
		}

		break;

	case CONTENT_STATUS_DONE:
		/* Nothing to do */
		break;

	default:
		NSLOG(netsurf, INFO, "Unexpected status %d (%p)", c->status,
		      c);
		assert(0);
	}
}


/**
 * Reformat a CONTENT_HTML to a new width.
 */

static void html_reformat(struct content *c, int width, int height)
{
	html_content *htmlc = (html_content *) c;
	struct box *layout;
	uint64_t ms_before;
	uint64_t ms_after;
	uint64_t ms_interval;

	nsu_getmonotonic_ms(&ms_before);

	htmlc->reflowing = true;

	htmlc->unit_len_ctx.viewport_width = css_unit_device2css_px(
			INTTOFIX(width), htmlc->unit_len_ctx.device_dpi);
	htmlc->unit_len_ctx.viewport_height = css_unit_device2css_px(
			INTTOFIX(height), htmlc->unit_len_ctx.device_dpi);
	htmlc->unit_len_ctx.root_style = htmlc->layout->style;

	layout_document(htmlc, width, height);
	layout = htmlc->layout;

	/* width and height are at least margin box of document */
	c->width = layout->x + layout->padding[LEFT] + layout->width +
		layout->padding[RIGHT] + layout->border[RIGHT].width +
		layout->margin[RIGHT];
	c->height = layout->y + layout->padding[TOP] + layout->height +
		layout->padding[BOTTOM] + layout->border[BOTTOM].width +
		layout->margin[BOTTOM];

	/* if boxes overflow right or bottom edge, expand to contain it */
	if (c->width < layout->x + layout->descendant_x1)
		c->width = layout->x + layout->descendant_x1;
	if (c->height < layout->y + layout->descendant_y1)
		c->height = layout->y + layout->descendant_y1;

	selection_reinit(htmlc->sel);

	htmlc->reflowing = false;
	htmlc->had_initial_layout = true;

	/* calculate next reflow time at three times what it took to reflow */
	nsu_getmonotonic_ms(&ms_after);

	ms_interval = (ms_after - ms_before) * 3;
	if (ms_interval < (nsoption_uint(min_reflow_period) * 10)) {
		ms_interval = nsoption_uint(min_reflow_period) * 10;
	}
	c->reformat_time = ms_after + ms_interval;
}


/**
 * Redraw a box.
 *
 * \param  h	content containing the box, of type CONTENT_HTML
 * \param  box  box to redraw
 */

void html_redraw_a_box(hlcache_handle *h, struct box *box)
{
	int x, y;

	box_coords(box, &x, &y);

	content_request_redraw(h, x, y,
			box->padding[LEFT] + box->width + box->padding[RIGHT],
			box->padding[TOP] + box->height + box->padding[BOTTOM]);
}


/**
 * Redraw a box.
 *
 * \param html  content containing the box, of type CONTENT_HTML
 * \param box  box to redraw.
 */

void html__redraw_a_box(struct html_content *html, struct box *box)
{
	int x, y;

	box_coords(box, &x, &y);

	content__request_redraw((struct content *)html, x, y,
			box->padding[LEFT] + box->width + box->padding[RIGHT],
			box->padding[TOP] + box->height + box->padding[BOTTOM]);
}

static void html_destroy_frameset(struct content_html_frames *frameset)
{
	int i;

	if (frameset->name) {
		talloc_free(frameset->name);
		frameset->name = NULL;
	}
	if (frameset->url) {
		talloc_free(frameset->url);
		frameset->url = NULL;
	}
	if (frameset->children) {
		for (i = 0; i < (frameset->rows * frameset->cols); i++) {
			if (frameset->children[i].name) {
				talloc_free(frameset->children[i].name);
				frameset->children[i].name = NULL;
			}
			if (frameset->children[i].url) {
				nsurl_unref(frameset->children[i].url);
				frameset->children[i].url = NULL;
			}
			if (frameset->children[i].children)
				html_destroy_frameset(&frameset->children[i]);
		}
		talloc_free(frameset->children);
		frameset->children = NULL;
	}
}

static void html_destroy_iframe(struct content_html_iframe *iframe)
{
	struct content_html_iframe *next;
	next = iframe;
	while ((iframe = next) != NULL) {
		next = iframe->next;
		if (iframe->name)
			talloc_free(iframe->name);
		if (iframe->url) {
			nsurl_unref(iframe->url);
			iframe->url = NULL;
		}
		talloc_free(iframe);
	}
}


static void html_free_layout(html_content *htmlc)
{
	if (htmlc->bctx != NULL) {
		/* freeing talloc context should let the entire box
		 * set be destroyed
		 */
		talloc_free(htmlc->bctx);
	}
}

/**
 * Destroy a CONTENT_HTML and free all resources it owns.
 */

static void html_destroy(struct content *c)
{
	html_content *html = (html_content *) c;
	struct form *f, *g;

	NSLOG(netsurf, INFO, "content %p", c);

#ifdef _NOVAOS
	html_cancel_relayout(html);
#endif

	/* If we're still converting a layout, cancel it */
	if (html->box_conversion_context != NULL) {
		if (cancel_dom_to_box(html->box_conversion_context) != NSERROR_OK) {
			NSLOG(netsurf, CRITICAL, "WARNING, Unable to cancel conversion context, browser may crash");
		}
	}

	selection_destroy(html->sel);

	/* Destroy forms */
	for (f = html->forms; f != NULL; f = g) {
		g = f->prev;

		form_free(f);
	}

	imagemap_destroy(html);

	if (c->refresh)
		nsurl_unref(c->refresh);

	if (html->base_url)
		nsurl_unref(html->base_url);

	/* At this point we can be moderately confident the JS is offline
	 * so we destroy the JS thread.
	 */
	if (html->jsthread != NULL) {
		js_destroythread(html->jsthread);
		html->jsthread = NULL;
	}

	if (html->parser != NULL) {
		dom_hubbub_parser_destroy(html->parser);
		html->parser = NULL;
	}

	if (html->document != NULL) {
		dom_node_unref(html->document);
		html->document = NULL;
	}

	if (html->title != NULL) {
		dom_node_unref(html->title);
		html->title = NULL;
	}

	/* Free encoding */
	if (html->encoding != NULL) {
		free(html->encoding);
		html->encoding = NULL;
	}

	/* Free base target */
	if (html->base_target != NULL) {
		free(html->base_target);
		html->base_target = NULL;
	}

	/* Free frameset */
	if (html->frameset != NULL) {
		html_destroy_frameset(html->frameset);
		talloc_free(html->frameset);
		html->frameset = NULL;
	}

	/* Free iframes */
	if (html->iframe != NULL) {
		html_destroy_iframe(html->iframe);
		html->iframe = NULL;
	}

	/* Destroy selection context */
	if (html->select_ctx != NULL) {
		css_select_ctx_destroy(html->select_ctx);
		html->select_ctx = NULL;
	}

	if (html->universal != NULL) {
		lwc_string_unref(html->universal);
		html->universal = NULL;
	}

	if (html->media.prefers_color_scheme != NULL) {
		lwc_string_unref(html->media.prefers_color_scheme);
		html->media.prefers_color_scheme = NULL;
	}

	/* Free stylesheets */
	html_css_free_stylesheets(html);

	/* Free scripts */
	html_script_free(html);

	/* Free objects */
	html_object_free_objects(html);

	/* free layout */
	html_free_layout(html);
}


static nserror html_clone(const struct content *old, struct content **newc)
{
	/** \todo Clone HTML specifics */

	/* In the meantime, we should never be called, as HTML contents
	 * cannot be shared and we're not intending to fix printing's
	 * cloning of documents. */
	assert(0 && "html_clone should never be called");

	return true;
}


/**
 * Handle a window containing a CONTENT_HTML being opened.
 */

static nserror
html_open(struct content *c,
	  struct browser_window *bw,
	  struct content *page,
	  struct object_params *params)
{
	html_content *html = (html_content *) c;

	html->bw = bw;
	html->page = (html_content *) page;

	html->drag_type = HTML_DRAG_NONE;
	html->drag_owner.no_owner = true;

	/* text selection */
	selection_init(html->sel);
	html->selection_type = HTML_SELECTION_NONE;
	html->selection_owner.none = true;

	html_object_open_objects(html, bw);

	return NSERROR_OK;
}


/**
 * Handle a window containing a CONTENT_HTML being closed.
 */

static nserror html_close(struct content *c)
{
	html_content *htmlc = (html_content *) c;
	nserror ret = NSERROR_OK;

	selection_clear(htmlc->sel, false);

	/* clear the html content reference to the browser window */
	htmlc->bw = NULL;

	/* remove all object references from the html content */
	html_object_close_objects(htmlc);

	if (htmlc->jsthread != NULL) {
		/* Close, but do not destroy (yet) the JS thread */
		ret = js_closethread(htmlc->jsthread);
	}

	return ret;
}


/**
 * Return an HTML content's selection context
 */

static void html_clear_selection(struct content *c)
{
	html_content *html = (html_content *) c;

	switch (html->selection_type) {
	case HTML_SELECTION_NONE:
		/* Nothing to do */
		assert(html->selection_owner.none == true);
		break;
	case HTML_SELECTION_TEXTAREA:
		textarea_clear_selection(html->selection_owner.textarea->
				gadget->data.text.ta);
		break;
	case HTML_SELECTION_SELF:
		assert(html->selection_owner.none == false);
		selection_clear(html->sel, true);
		break;
	case HTML_SELECTION_CONTENT:
		content_clear_selection(html->selection_owner.content->object);
		break;
	default:
		break;
	}

	/* There is no selection now. */
	html->selection_type = HTML_SELECTION_NONE;
	html->selection_owner.none = true;
}


/**
 * Return an HTML content's selection context
 */

static char *html_get_selection(struct content *c)
{
	html_content *html = (html_content *) c;

	switch (html->selection_type) {
	case HTML_SELECTION_TEXTAREA:
		return textarea_get_selection(html->selection_owner.textarea->
				gadget->data.text.ta);
	case HTML_SELECTION_SELF:
		assert(html->selection_owner.none == false);
		return selection_get_copy(html->sel);
	case HTML_SELECTION_CONTENT:
		return content_get_selection(
				html->selection_owner.content->object);
	case HTML_SELECTION_NONE:
		/* Nothing to do */
		assert(html->selection_owner.none == true);
		break;
	default:
		break;
	}

	return NULL;
}


/**
 * Get access to any content, link URLs and objects (images) currently
 * at the given (x, y) coordinates.
 *
 * \param[in] c html content to look inside
 * \param[in] x x-coordinate of point of interest
 * \param[in] y y-coordinate of point of interest
 * \param[out] data Positional features struct to be updated with any
 *             relevent content, or set to NULL if none.
 * \return NSERROR_OK on success else appropriate error code.
 */
static nserror
html_get_contextual_content(struct content *c, int x, int y,
			    struct browser_window_features *data)
{
	html_content *html = (html_content *) c;

	struct box *box = html->layout;
	struct box *next;
	int box_x = 0, box_y = 0;

	while ((next = box_at_point(&html->unit_len_ctx, box, x, y,
			&box_x, &box_y)) != NULL) {
		box = next;

		/* hidden boxes are ignored */
		if ((box->style != NULL) &&
		    css_computed_visibility(box->style) == CSS_VISIBILITY_HIDDEN) {
			continue;
		}

		if (box->iframe) {
			float scale = browser_window_get_scale(box->iframe);
			browser_window_get_features(box->iframe,
						    (x - box_x) * scale,
						    (y - box_y) * scale,
						    data);
		}

		if (box->object)
			content_get_contextual_content(box->object,
					x - box_x, y - box_y, data);

		if (box->object)
			data->object = box->object;

		if (box->href)
			data->link = box->href;

		if (box->usemap) {
			const char *target = NULL;
			nsurl *url = imagemap_get(html, box->usemap, box_x,
					box_y, x, y, &target);
			/* Box might have imagemap, but no actual link area
			 * at point */
			if (url != NULL)
				data->link = url;
		}
		if (box->gadget) {
			switch (box->gadget->type) {
			case GADGET_TEXTBOX:
			case GADGET_TEXTAREA:
			case GADGET_PASSWORD:
				data->form_features = CTX_FORM_TEXT;
				break;

			case GADGET_FILE:
				data->form_features = CTX_FORM_FILE;
				break;

			default:
				data->form_features = CTX_FORM_NONE;
				break;
			}
		}
	}
	return NSERROR_OK;
}


/**
 * Scroll deepest thing within the content which can be scrolled at given point
 *
 * \param c	html content to look inside
 * \param x	x-coordinate of point of interest
 * \param y	y-coordinate of point of interest
 * \param scrx	number of px try to scroll something in x direction
 * \param scry	number of px try to scroll something in y direction
 * \return true iff scroll was consumed by something in the content
 */
static bool
html_scroll_at_point(struct content *c, int x, int y, int scrx, int scry)
{
	html_content *html = (html_content *) c;

	struct box *box = html->layout;
	struct box *next;
	int box_x = 0, box_y = 0;
	bool handled_scroll = false;

	/* TODO: invert order; visit deepest box first */

	while ((next = box_at_point(&html->unit_len_ctx, box, x, y,
			&box_x, &box_y)) != NULL) {
		box = next;

		if (box->style && css_computed_visibility(box->style) ==
				CSS_VISIBILITY_HIDDEN)
			continue;

		/* Pass into iframe */
		if (box->iframe) {
			float scale = browser_window_get_scale(box->iframe);

			if (browser_window_scroll_at_point(box->iframe,
							   (x - box_x) * scale,
							   (y - box_y) * scale,
							   scrx, scry) == true)
				return true;
		}

		/* Pass into textarea widget */
		if (box->gadget && (box->gadget->type == GADGET_TEXTAREA ||
				box->gadget->type == GADGET_PASSWORD ||
				box->gadget->type == GADGET_TEXTBOX) &&
				textarea_scroll(box->gadget->data.text.ta,
						scrx, scry) == true)
			return true;

		/* Pass into object */
		if (box->object != NULL && content_scroll_at_point(
				box->object, x - box_x, y - box_y,
				scrx, scry) == true)
			return true;

		/* Handle box scrollbars */
		if (box->scroll_y && scrollbar_scroll(box->scroll_y, scry))
			handled_scroll = true;

		if (box->scroll_x && scrollbar_scroll(box->scroll_x, scrx))
			handled_scroll = true;

		if (handled_scroll == true)
			return true;
	}

	return false;
}

/** Helper for file gadgets to store their filename unencoded on the
 * dom node associated with the gadget.
 *
 * \todo Get rid of this crap eventually
 */
static void html__dom_user_data_handler(dom_node_operation operation,
		dom_string *key, void *_data, struct dom_node *src,
		struct dom_node *dst)
{
	char *oldfile;
	char *data = (char *)_data;

	if (!dom_string_isequal(corestring_dom___ns_key_file_name_node_data,
				key) || data == NULL) {
		return;
	}

	switch (operation) {
	case DOM_NODE_CLONED:
		if (dom_node_set_user_data(dst,
					   corestring_dom___ns_key_file_name_node_data,
					   strdup(data), html__dom_user_data_handler,
					   &oldfile) == DOM_NO_ERR) {
			if (oldfile != NULL)
				free(oldfile);
		}
		break;

	case DOM_NODE_RENAMED:
	case DOM_NODE_IMPORTED:
	case DOM_NODE_ADOPTED:
		break;

	case DOM_NODE_DELETED:
		free(data);
		break;
	default:
		NSLOG(netsurf, INFO, "User data operation not handled.");
		assert(0);
	}
}

static void html__set_file_gadget_filename(struct content *c,
	struct form_control *gadget, const char *fn)
{
	nserror ret;
	char *utf8_fn, *oldfile = NULL;
	html_content *html = (html_content *)c;
	struct box *file_box = gadget->box;

	ret = guit->utf8->local_to_utf8(fn, 0, &utf8_fn);
	if (ret != NSERROR_OK) {
		assert(ret != NSERROR_BAD_ENCODING);
		NSLOG(netsurf, INFO,
		      "utf8 to local encoding conversion failed");
		/* Load was for us - just no memory */
		return;
	}

	form_gadget_update_value(gadget, utf8_fn);

	/* corestring_dom___ns_key_file_name_node_data */
	if (dom_node_set_user_data((dom_node *)file_box->gadget->node,
				   corestring_dom___ns_key_file_name_node_data,
				   strdup(fn), html__dom_user_data_handler,
				   &oldfile) == DOM_NO_ERR) {
		if (oldfile != NULL)
			free(oldfile);
	}

	/* Redraw box. */
	html__redraw_a_box(html, file_box);
}

void html_set_file_gadget_filename(struct hlcache_handle *hl,
	struct form_control *gadget, const char *fn)
{
	return html__set_file_gadget_filename(hlcache_handle_get_content(hl),
		gadget, fn);
}

/**
 * Drop a file onto a content at a particular point, or determine if a file
 * may be dropped onto the content at given point.
 *
 * \param c	html content to look inside
 * \param x	x-coordinate of point of interest
 * \param y	y-coordinate of point of interest
 * \param file	path to file to be dropped, or NULL to know if drop allowed
 * \return true iff file drop has been handled, or if drop possible (NULL file)
 */
static bool html_drop_file_at_point(struct content *c, int x, int y, char *file)
{
	html_content *html = (html_content *) c;

	struct box *box = html->layout;
	struct box *next;
	struct box *file_box = NULL;
	struct box *text_box = NULL;
	int box_x = 0, box_y = 0;

	/* Scan box tree for boxes that can handle drop */
	while ((next = box_at_point(&html->unit_len_ctx, box, x, y,
			&box_x, &box_y)) != NULL) {
		box = next;

		if (box->style &&
		    css_computed_visibility(box->style) == CSS_VISIBILITY_HIDDEN)
			continue;

		if (box->iframe) {
			float scale = browser_window_get_scale(box->iframe);
			return browser_window_drop_file_at_point(
				box->iframe,
				(x - box_x) * scale,
				(y - box_y) * scale,
				file);
		}

		if (box->object &&
		    content_drop_file_at_point(box->object,
					x - box_x, y - box_y, file) == true)
			return true;

		if (box->gadget) {
			switch (box->gadget->type) {
				case GADGET_FILE:
					file_box = box;
				break;

				case GADGET_TEXTBOX:
				case GADGET_TEXTAREA:
				case GADGET_PASSWORD:
					text_box = box;
					break;

				default:	/* appease compiler */
					break;
			}
		}
	}

	if (!file_box && !text_box)
		/* No box capable of handling drop */
		return false;

	if (file == NULL)
		/* There is a box capable of handling drop here */
		return true;

	/* Handle the drop */
	if (file_box) {
		/* File dropped on file input */
		html__set_file_gadget_filename(c, file_box->gadget, file);

	} else {
		/* File dropped on text input */

		size_t file_len;
		FILE *fp = NULL;
		char *buffer;
		char *utf8_buff;
		nserror ret;
		unsigned int size;
		int bx, by;

		/* Open file */
		fp = fopen(file, "rb");
		if (fp == NULL) {
			/* Couldn't open file, but drop was for us */
			return true;
		}

		/* Get filesize */
		fseek(fp, 0, SEEK_END);
		file_len = ftell(fp);
		fseek(fp, 0, SEEK_SET);

		if ((long)file_len == -1) {
			/* unable to get file length, but drop was for us */
			fclose(fp);
			return true;
		}

		/* Allocate buffer for file data */
		buffer = malloc(file_len + 1);
		if (buffer == NULL) {
			/* No memory, but drop was for us */
			fclose(fp);
			return true;
		}

		/* Stick file into buffer */
		if (file_len != fread(buffer, 1, file_len, fp)) {
			/* Failed, but drop was for us */
			free(buffer);
			fclose(fp);
			return true;
		}

		/* Done with file */
		fclose(fp);

		/* Ensure buffer's string termination */
		buffer[file_len] = '\0';

		/* TODO: Sniff for text? */

		/* Convert to UTF-8 */
		ret = guit->utf8->local_to_utf8(buffer, file_len, &utf8_buff);
		if (ret != NSERROR_OK) {
			/* bad encoding shouldn't happen */
			NSLOG(netsurf, ERROR,
			      "local to utf8 encoding failed (%s)",
			      messages_get_errorcode(ret));
			assert(ret != NSERROR_BAD_ENCODING);
			free(buffer);
			return true;
		}

		/* Done with buffer */
		free(buffer);

		/* Get new length */
		size = strlen(utf8_buff);

		/* Simulate a click over the input box, to place caret */
		box_coords(text_box, &bx, &by);
		textarea_mouse_action(text_box->gadget->data.text.ta,
				BROWSER_MOUSE_PRESS_1, x - bx, y - by);

		/* Paste the file as text */
		textarea_drop_text(text_box->gadget->data.text.ta,
				utf8_buff, size);

		free(utf8_buff);
	}

	return true;
}


/**
 * set debug status.
 *
 * \param c The content to debug
 * \param op The debug operation type
 */
static nserror
html_debug(struct content *c, enum content_debug op)
{
	html_redraw_debug = !html_redraw_debug;

	return NSERROR_OK;
}


/**
 * Dump debug info concerning the html_content
 *
 * \param c The content to debug
 * \param f The file to dump to
 * \param op The debug dump type
 */
static nserror
html_debug_dump(struct content *c, FILE *f, enum content_debug op)
{
	html_content *htmlc = (html_content *)c;
	dom_node *html;
	dom_exception exc; /* returned by libdom functions */
	nserror ret;

	assert(htmlc != NULL);

	if (op == CONTENT_DEBUG_RENDER) {
		assert(htmlc->layout != NULL);
		box_dump(f, htmlc->layout, 0, true);
		ret = NSERROR_OK;
	} else {
		if (htmlc->document == NULL) {
			NSLOG(netsurf, INFO, "No document to dump");
			return NSERROR_DOM;
		}

		exc = dom_document_get_document_element(htmlc->document, (void *) &html);
		if ((exc != DOM_NO_ERR) || (html == NULL)) {
			NSLOG(netsurf, INFO, "Unable to obtain root node");
			return NSERROR_DOM;
		}

		ret = libdom_dump_structure(html, f, 0);

		NSLOG(netsurf, INFO, "DOM structure dump returning %d", ret);

		dom_node_unref(html);
	}

	return ret;
}


#if ALWAYS_DUMP_FRAMESET
/**
 * Print a frameset tree to stderr.
 */

static void
html_dump_frameset(struct content_html_frames *frame, unsigned int depth)
{
	unsigned int i;
	int row, col, index;
	const char *unit[] = {"px", "%", "*"};
	const char *scrolling[] = {"auto", "yes", "no"};

	assert(frame);

	fprintf(stderr, "%p ", frame);

	fprintf(stderr, "(%i %i) ", frame->rows, frame->cols);

	fprintf(stderr, "w%g%s ", frame->width.value, unit[frame->width.unit]);
	fprintf(stderr, "h%g%s ", frame->height.value,unit[frame->height.unit]);
	fprintf(stderr, "(margin w%i h%i) ",
			frame->margin_width, frame->margin_height);

	if (frame->name)
		fprintf(stderr, "'%s' ", frame->name);
	if (frame->url)
		fprintf(stderr, "<%s> ", frame->url);

	if (frame->no_resize)
		fprintf(stderr, "noresize ");
	fprintf(stderr, "(scrolling %s) ", scrolling[frame->scrolling]);
	if (frame->border)
		fprintf(stderr, "border %x ",
				(unsigned int) frame->border_colour);

	fprintf(stderr, "\n");

	if (frame->children) {
		for (row = 0; row != frame->rows; row++) {
			for (col = 0; col != frame->cols; col++) {
				for (i = 0; i != depth; i++)
					fprintf(stderr, "  ");
				fprintf(stderr, "(%i %i): ", row, col);
				index = (row * frame->cols) + col;
				html_dump_frameset(&frame->children[index],
						depth + 1);
			}
		}
	}
}

#endif

/**
 * Retrieve HTML document tree
 *
 * \param h  HTML content to retrieve document tree from
 * \return Pointer to document tree
 */
dom_document *html_get_document(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->document;
}

/**
 * Retrieve box tree
 *
 * \param h  HTML content to retrieve tree from
 * \return Pointer to box tree
 *
 * \todo This API must die, as must all use of the box tree outside of
 *         HTML content handler
 */
struct box *html_get_box_tree(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->layout;
}

/**
 * Retrieve the charset of an HTML document
 *
 * \param c Content to retrieve charset from
 * \param op The content encoding operation to perform.
 * \return Pointer to charset, or NULL
 */
static const char *html_encoding(const struct content *c, enum content_encoding_type op)
{
	html_content *html = (html_content *) c;
	static char enc_token[10] = "Encoding0";

	assert(html != NULL);

	if (op == CONTENT_ENCODING_SOURCE) {
		enc_token[8] = '0' + html->encoding_source;
		return messages_get(enc_token);
	}

	return html->encoding;
}


/**
 * Retrieve framesets used in an HTML document
 *
 * \param h  Content to inspect
 * \return Pointer to framesets, or NULL if none
 */
struct content_html_frames *html_get_frameset(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->frameset;
}

/**
 * Retrieve iframes used in an HTML document
 *
 * \param h  Content to inspect
 * \return Pointer to iframes, or NULL if none
 */
struct content_html_iframe *html_get_iframe(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->iframe;
}

/**
 * Retrieve an HTML content's base URL
 *
 * \param h  Content to retrieve base target from
 * \return Pointer to URL
 */
nsurl *html_get_base_url(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->base_url;
}

/**
 * Retrieve an HTML content's base target
 *
 * \param h  Content to retrieve base target from
 * \return Pointer to target, or NULL if none
 */
const char *html_get_base_target(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->base_target;
}


/**
 * Retrieve layout coordinates of box with given id
 *
 * \param h        HTML document to search
 * \param frag_id  String containing an element id
 * \param x        Updated to global x coord iff id found
 * \param y        Updated to global y coord iff id found
 * \return  true iff id found
 */
bool html_get_id_offset(hlcache_handle *h, lwc_string *frag_id, int *x, int *y)
{
	struct box *pos;
	struct box *layout;

	if (content_get_type(h) != CONTENT_HTML)
		return false;

	layout = html_get_box_tree(h);

	if ((pos = box_find_by_id(layout, frag_id)) != 0) {
		box_coords(pos, x, y);
		return true;
	}
	return false;
}

bool html_exec(struct content *c, const char *src, size_t srclen)
{
	html_content *htmlc = (html_content *)c;
	bool result = false;
	dom_exception err;
	dom_html_body_element *body_node;
	dom_string *dom_src;
	dom_text *text_node;
	dom_node *spare_node;
	dom_html_script_element *script_node;

	if (htmlc->document == NULL) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, no document");
		goto out_no_string;
	}

	err = dom_string_create((const uint8_t *)src, srclen, &dom_src);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not create string");
		goto out_no_string;
	}

	err = dom_html_document_get_body(htmlc->document, &body_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to retrieve body element");
		goto out_no_body;
	}

	err = dom_document_create_text_node(htmlc->document, dom_src, &text_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not create text node");
		goto out_no_text_node;
	}

	err = dom_document_create_element(htmlc->document, corestring_dom_SCRIPT, &script_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not create script node");
		goto out_no_script_node;
	}

	err = dom_node_append_child(script_node, text_node, &spare_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not insert code node into script node");
		goto out_unparented;
	}
	dom_node_unref(spare_node); /* We do not need the spare ref at all */

	err = dom_node_append_child(body_node, script_node, &spare_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not insert script node into document body");
		goto out_unparented;
	}
	dom_node_unref(spare_node); /* Again no need for the spare ref */

	/* We successfully inserted the node into the DOM */

	result = true;

	/* Now we unwind, starting by removing the script from wherever it
	 * ended up parented
	 */

	err = dom_node_get_parent_node(script_node, &spare_node);
	if (err == DOM_NO_ERR && spare_node != NULL) {
		dom_node *second_spare;
		err = dom_node_remove_child(spare_node, script_node, &second_spare);
		if (err == DOM_NO_ERR) {
			dom_node_unref(second_spare);
		}
		dom_node_unref(spare_node);
	}

out_unparented:
	dom_node_unref(script_node);
out_no_script_node:
	dom_node_unref(text_node);
out_no_text_node:
	dom_node_unref(body_node);
out_no_body:
	dom_string_unref(dom_src);
out_no_string:
	return result;
}

/* See \ref content_saw_insecure_objects */
static bool
html_saw_insecure_objects(struct content *c)
{
	html_content *htmlc = (html_content *)c;
	struct content_html_object *obj = htmlc->object_list;

	/* Check through the object list */
	while (obj != NULL) {
		if (obj->content != NULL) {
			if (content_saw_insecure_objects(obj->content))
				return true;
		}
		obj = obj->next;
	}

	/* Now check the script list */
	if (html_saw_insecure_scripts(htmlc)) {
		return true;
	}

	/* Now check stylesheets */
	if (html_css_saw_insecure_stylesheets(htmlc)) {
		return true;
	}

	return false;
}

/**
 * Compute the type of a content
 *
 * \return CONTENT_HTML
 */
static content_type html_content_type(void)
{
	return CONTENT_HTML;
}


static void html_fini(void)
{
	html_css_fini();
}

/**
 * Finds all occurrences of a given string in an html box
 *
 * \param pattern   the string pattern to search for
 * \param p_len     pattern length
 * \param cur       pointer to the current box
 * \param case_sens whether to perform a case sensitive search
 * \param context   The search context to add the entry to.
 * \return true on success, false on memory allocation failure
 */
static nserror
find_occurrences_html_box(const char *pattern,
			  int p_len,
			  struct box *cur,
			  bool case_sens,
			  struct textsearch_context *context)
{
	struct box *a;
	nserror res = NSERROR_OK;

	/* ignore this box, if there's no visible text */
	if (!cur->object && cur->text) {
		const char *text = cur->text;
		unsigned length = cur->length;

		while (length > 0) {
			unsigned match_length;
			unsigned match_offset;
			const char *new_text;
			const char *pos;

			pos = content_textsearch_find_pattern(text,
					   length,
					   pattern,
					   p_len,
					   case_sens,
					   &match_length);
			if (!pos)
				break;

			/* found string in box => add to list */
			match_offset = pos - cur->text;

			res = content_textsearch_add_match(context,
					cur->byte_offset + match_offset,
					cur->byte_offset + match_offset + match_length,
					cur,
					cur);
			if (res != NSERROR_OK) {
				return res;
			}

			new_text = pos + match_length;
			length -= (new_text - text);
			text = new_text;
		}
	}

	/* and recurse */
	for (a = cur->children; a; a = a->next) {
		res = find_occurrences_html_box(pattern,
						p_len,
						a,
						case_sens,
						context);
		if (res != NSERROR_OK) {
			return res;
		}
	}

	return res;
}

/**
 * Finds all occurrences of a given string in the html box tree
 *
 * \param pattern   the string pattern to search for
 * \param p_len     pattern length
 * \param c The content to search
 * \param csens whether to perform a case sensitive search
 * \param context   The search context to add the entry to.
 * \return true on success, false on memory allocation failure
 */
static nserror
html_textsearch_find(struct content *c,
		     struct textsearch_context *context,
		     const char *pattern,
		     int p_len,
		     bool csens)
{
	html_content *html = (html_content *)c;

	if (html->layout == NULL) {
		return NSERROR_INVALID;
	}

	return find_occurrences_html_box(pattern,
					 p_len,
					 html->layout,
					 csens,
					 context);
}


static nserror
html_textsearch_bounds(struct content *c,
		       unsigned start_idx,
		       unsigned end_idx,
		       struct box *start_box,
		       struct box *end_box,
		       struct rect *bounds)
{
	/* get box position and jump to it */
	box_coords(start_box, &bounds->x0, &bounds->y0);
	/* \todo: move x0 in by correct idx */
	box_coords(end_box, &bounds->x1, &bounds->y1);
	/* \todo: move x1 in by correct idx */
	bounds->x1 += end_box->width;
	bounds->y1 += end_box->height;

	return NSERROR_OK;
}


/**
 * HTML content handler function table
 */
static const content_handler html_content_handler = {
	.fini = html_fini,
	.create = html_create,
	.process_data = html_process_data,
	.data_complete = html_convert,
	.reformat = html_reformat,
	.destroy = html_destroy,
	.stop = html_stop,
	.mouse_track = html_mouse_track,
	.mouse_action = html_mouse_action,
	.keypress = html_keypress,
	.redraw = html_redraw,
	.open = html_open,
	.close = html_close,
	.get_selection = html_get_selection,
	.clear_selection = html_clear_selection,
	.get_contextual_content = html_get_contextual_content,
	.scroll_at_point = html_scroll_at_point,
	.drop_file_at_point = html_drop_file_at_point,
	.debug_dump = html_debug_dump,
	.debug = html_debug,
	.clone = html_clone,
	.get_encoding = html_encoding,
	.type = html_content_type,
	.exec = html_exec,
	.saw_insecure_objects = html_saw_insecure_objects,
	.textsearch_find = html_textsearch_find,
	.textsearch_bounds = html_textsearch_bounds,
	.textselection_redraw = html_textselection_redraw,
	.textselection_copy = html_textselection_copy,
	.textselection_get_end = html_textselection_get_end,
	.no_share = true,
};


/* exported function documented in html/html.h */
nserror html_init(void)
{
	uint32_t i;
	nserror error;

	error = html_css_init();
	if (error != NSERROR_OK)
		goto error;

	for (i = 0; i < NOF_ELEMENTS(html_types); i++) {
		error = content_factory_register_handler(html_types[i],
				&html_content_handler);
		if (error != NSERROR_OK)
			goto error;
	}

	return NSERROR_OK;

error:
	html_fini();

	return error;
}
