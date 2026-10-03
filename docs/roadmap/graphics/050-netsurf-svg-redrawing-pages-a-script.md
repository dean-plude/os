- ~~NetSurf: SVG; redrawing pages a script changes after layout.~~ Done
  (Phase 19.8): SVG images (`<img>`, `<object>`, CSS backgrounds, `.svg`
  pages) and `<svg>` elements written inline in HTML drawn anti-aliased,
  and pages laid out again when a script changes the DOM, a style or a
  stylesheet (only the changed part is built again, and the layout starts
  at the changed box).  Still to do: SVG text in the document's fonts.
