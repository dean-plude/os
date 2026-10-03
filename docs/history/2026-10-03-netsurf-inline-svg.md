## NetSurf: inline SVG

Phase 19.8 is finished: NetSurf draws an `<svg>` element written inline
in a page's HTML, not only SVG files.

- **Inline `<svg>`** is drawn like an image (`box_inline_svg` in
  `content/handlers/html/box_special.c`): its subtree is written out as an
  SVG document (with the SVG and XLink namespaces the HTML parser keeps on
  the nodes, and SVG's mixed-case names such as `viewBox` and
  `linearGradient`, which libdom stores in lower case) and fetched as a `data:image/svg+xml` URL, so the same
  libsvgtiny parser and plutovg path plotter that draw SVG files draw it.
  Its shapes never become boxes of their own.  The element's `width` and
  `height` attributes size it like an `<img>`'s (`css/hints.c`), CSS
  overrides them, and the `viewBox` scales the drawing into the box.  A
  script changing the SVG's DOM lays the page out again like any other
  change; an unchanged SVG keeps its image across the rebuild.
- **Test**: `nstest`'s page now also carries an inline SVG (a square and a
  circle, its `viewBox` scaled 4x) and a list a script builds with
  `createElement('li')` in a loop; the test checks both shapes' colours
  and sizes and the three list items, before and after the click that
  lays the page out again.
