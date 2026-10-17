## XAML's collections

Fifth part of step 32.3 of `Windows.UI.Xaml` on NovaOS (Phase 32,
[design](../design/windows-ui-xaml.md)): the XAML core's collections now
build from Microsoft's sources and link, with the layers under them.

- **Built and linked** in `xaml_components.lib`: the collections
  (`Collection`, 16 sources), `CDependencyObject` with its minimal
  library, and the value types under them (`CValue`, flyweight,
  primitive dependency objects, `math`, `theming`).
- **Stand-ins:** the composition tree's `CompHelper/CompositionHelper.h`
  and `CompositionVisualSurfaceHelper.h` (declare-only), and
  `CoreWindowSiteBridge` in NovaOS's lifted IDL.
- **Microsoft's unit-test stubs and metadata mocks** stand in for the
  layers above (`xaml_teststubs.lib`, linked by `xamlbasetest` only).
- **Below XAML:** `thread_local` destructors (`__tlregdtor`), the UCRT's
  `_MAX_*TOSTR_*_COUNT` sizes, and `windowsnumerics.h`'s `invert`.
- A self-test program's manifest can now add compiler flags (`"cflags"`).

`xamlbasetest` now also fills, searches and empties a
`DependencyObjectCollection` and walks a `CollectionMoveView`.
