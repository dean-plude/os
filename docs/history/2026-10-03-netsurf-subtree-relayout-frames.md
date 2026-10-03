## NetSurf: subtree relayout, iframes on changing pages, unsized SVGs

The three NetSurf leftovers from Phase 19.8.

- **Only the changed part of a page is built again.**  A script's change
  used to throw the whole box tree away and build it again.  Now the
  change is recorded at the node whose children changed (libdom's
  `DOMSubtreeModified`; for a changed attribute, `DOMAttrModified`, the
  element's parent, since sibling selectors can restyle its later
  siblings), a burst of changes is merged into the lowest node above all
  of them, and `html_relayout` (`content/handlers/html/html.c`) rebuilds
  only the boxes below the nearest block, inline-block or table-cell box
  above it (`dom_to_box_subtree` in `box_construct.c`, which runs the
  same element and text construction as the first build, `:before`,
  `:after` and normalisation included).  Only those nodes' styles are
  selected again, and only that box and the boxes above it have their
  widths measured again; the rest of the page keeps its boxes, styles and
  cached widths.  A new stylesheet or a change reaching the root element
  still rebuilds everything, and a change inside `<head>` rebuilds
  nothing.  The rebuilt boxes live in a talloc context of their own,
  freed when that part of the page is built again, so a page a script
  keeps changing does not grow.  Nodes a script removes forget their
  boxes at once (`DOMNodeRemoved`), so a node put back elsewhere never
  points at a freed box.
  On a page of 1,500 styled paragraphs where a timer changes one line's
  text, each change took about 600 ms to rebuild plus 130 ms to lay out
  (TCG, 14 changes); now the rebuild is under the 10 ms timer's
  resolution and the layout about 80 ms (15 changes), so a change costs
  about 80 ms instead of 730 ms.  The positioning pass of layout still
  walks the whole page.
- **Pages with iframes are laid out again too.**  These were skipped
  before, because each iframe's browser window hangs off its box.  Now
  the windows are unlinked from the boxes about to go and linked to the
  new boxes of the same elements afterwards
  (`browser_window_unlink_iframes` and `browser_window_relink_iframes` in
  `desktop/frames.c`), so an iframe keeps its page, scroll position and
  state across a rebuild; only when the page's iframes changed (one added
  or removed, or a new `src`) are they all opened again.  Framesets and
  iframes themselves already showed (NetSurf's own frame model: each
  frame is a browser window of its own); frameset pages have no boxes and
  keep the static layout.
- **An SVG without a width and height** drew at no size (a 0 x 0
  viewport when the `<img>` gave none).  libsvgtiny now gives it the
  default size of a replaced element, 300 x 150 CSS pixels, as other
  browsers do, with a missing side following the `viewBox`'s aspect
  ratio (`initialise_parse_state` in `libsvgtiny/src/svgtiny.c`).
- **Parser debug output**: libhubbub printed the tree builder's mode for
  every token to the console (its debug hook is on without `NDEBUG`);
  it is off on NovaOS.
- **Test**: `nstest`'s page also carries an SVG image without a size
  (checked at 300 x 150) and an iframe showing a frameset page with two
  coloured frames, all checked before and after the click that changes
  the page.
