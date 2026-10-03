## NetSurf: laying a page out again from the box a script changed

After a script changes a page, NetSurf laid out the whole page again,
even though only one part of it had been built again (#123): the line
layout of every paragraph, then the passes that place positioned boxes,
apply relative offsets and measure what each box's descendants cover.
Now the layout starts at the box whose children were built again.

- **The flow starts at the changed box.**  Each block formatting
  context's loop (`layout_block_context` in
  `content/handlers/html/layout.c`) records, for every box it visits, its
  state when it reaches the box and when it leaves it: the position, the
  margins still collapsing and whether the context has floats.  A layout
  after a change starts the loop at the changed box in the state it
  reached that box in last time (and, for a change inside an
  `overflow` box, does the same in each context on the way down), lays
  the box out again, and carries on with the boxes after it.  A later box
  the loop reaches in the same state as last time, apart from its
  position, would come out the same, only moved: it is moved as a whole
  and the loop continues from the state it left the box in, so only the
  changed box, the boxes above it and the positions of the boxes after
  it are worked out again.  Boxes before the change are not visited.
- **The passes after the flow** (list markers, positioned boxes,
  relative offsets, the overflow extents and iframe positions) visit only
  the boxes laid out again and the boxes above them.  When the change
  moved or resized anything and the page has absolutely positioned or
  fixed boxes, every positioned box is placed again from the static
  position the flow last gave it, since its containing block or its place
  may have changed.
- **What still lays out the whole page:** a change in a box with floats
  in its formatting context (floats placed before the change decide
  where later lines go), a change below a float, an inline-block, a table
  cell, a flex item or a positioned box, a change in a box whose top
  margin collapses into its first child's (a box with no border, padding
  or text before its first child box), a box above the change
  getting a height it did not have, positioned boxes on a page whose
  relative offsets moved floats or an inline's contents, a new window
  size, and a new stylesheet (which rebuilds every box anyway).  Floats
  inside a box that has a formatting context of its own (`overflow`
  other than `visible`) do not count: that box is moved as a whole.
- **Numbers** (TCG, the long page of `nstest`: 1,500 styled paragraphs,
  the fifth changed twelve times between one and three lines): the layout
  after a change takes 6.6 ms instead of 38.8 ms.  Of the full layout's
  38.8 ms, the flow (line layout and positions) took 29.0 ms and the
  passes after it 8.8 ms; from the changed box, the flow takes 1.5 ms and
  the passes 2.7 ms (they and the width measurement still visit each
  child of the boxes above the change, here the body's 1,500
  paragraphs).  With #123's rebuild of the changed part, a change on that
  page now costs about 7 ms of layout where it cost 730 ms before #123.
- **Checks**: `NETSURF_LAYOUT_LOG=file` makes NetSurf append a line per
  script-driven layout with its times (rebuild, widths, flow, placing)
  and the boxes laid out and moved; with `NETSURF_LAYOUT_CHECK=1` every
  third layout from a changed box is followed by a full layout and every
  box's position, size and overflow extent is compared;
  `NETSURF_LAYOUT=full` always lays out the whole page.
- **Test**: after its first page, `nstest` opens four more: the long
  page, a page with iframes below the changed paragraph, one with fixed,
  absolutely and relatively positioned boxes and one with floats in boxes
  of their own.  Each is shown twice while a script changes it twelve
  times, once laid out from the changed box and once with the check; the
  test requires every check to find no difference, the screenshots of
  the two runs (`nstest-PAGE-incremental.png`, `nstest-PAGE-check.png`)
  to be identical above the status bar, and the long page's layout from
  the changed box to take less than half the full layout's time.
