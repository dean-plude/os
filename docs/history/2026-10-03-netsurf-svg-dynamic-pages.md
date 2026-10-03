## NetSurf: SVG and pages scripts change

Phase 19.8.  NetSurf shows SVG images, and a page a script changes after
it was laid out is laid out and drawn again.

- **SVG** through NetSurf's own libsvgtiny 0.1.8 (MIT,
  `third_party/netsurf/libsvgtiny`), which parses with libdom's XML binding
  on expat 2.7.1 (MIT, `third_party/netsurf/libexpat`): `<img>`,
  `<object>`, CSS backgrounds and `.svg` files opened directly.  Upstream's
  framebuffer frontend had no path plotter ("path unimplemented"), so the
  shapes never showed; NovaOS's (`frontends/framebuffer/framebuffer.c`)
  fills and strokes each path with plutovg (MIT, already in the tree for
  GDI+), anti-aliased, in a scratch surface the size of the shape within
  the clip rectangle, and blends it onto the window's bitmap.
- **Pages a script changes**: NetSurf 3.11 builds the box tree once ("NS
  layout is static"), so text, elements, `style` or `class` attributes or
  stylesheets a script changed after layout never showed.  Now a change to
  the document (libdom's `DOMSubtreeModified`, outside form fields,
  scripts and the title) or a stylesheet arriving late schedules a rebuild
  (`html_relayout` in `content/handlers/html/html.c`): a burst of changes
  coalesces, the box tree is thrown away and built again from the DOM in
  one go (`dom_to_box_sync`), with a fresh CSS selection context and
  libcss's per-node caches dropped, then the page is reformatted at its
  width and redrawn.  Form fields keep their values (they live in the DOM);
  images are fetched again from the cache; the text selection, a drag and
  the caret are dropped.  Pages with frames or iframes keep the static
  layout.
- **SVG images parsed again** on each reformat (a window resize) went
  into the old diagram, so every shape was drawn once more each time;
  now each parse starts from an empty diagram.
- **Alt+F4** closes NetSurf: the window's `WM_CLOSE` (which `SC_CLOSE`
  sends rather than posts) is a quit event now, not only a posted one
  (`userland/netsurf/nsfb_novaos.c`).
- **Test**: `nstest` in the graphics self-tests
  (`tests/selftest/graphics/060-nstest.py`) opens a page with an SVG and a
  box whose `onclick` script changes it; the test checks the screen before
  and after clicking it.
- Not yet: inline `<svg>` elements in HTML (NetSurf draws only SVG files),
  and SVG text, which libsvgtiny places but NetSurf draws in a fixed size.
