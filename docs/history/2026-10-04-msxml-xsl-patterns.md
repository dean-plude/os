## MSXML: XSL Patterns match names as written

The Visual C++ Redistributable's installer (a WiX Burn bundle, which GOG
GALAXY needs for `mfc140u.dll`) extracted its manifest and then stopped
with 0x80070490, "Failed to select user experience node".  Burn creates
`Msxml2.DOMDocument`, MSXML 3's class, sets no selection property and asks
for `UX`, `Payload`, `Chain/MsiPackage` and so on, unprefixed, in a
manifest whose elements are all in a default namespace.  MSXML 3's
default selection language is XSL Patterns, which name an element by its
qualified name as written in the document, so those match; `msxml6.dll`
ran every query as XPath 1.0, where an unprefixed name means no namespace,
so nothing did.

- **XSL Patterns in `msxml6.dll`** (`userland/msxml6/dom.c`): when a
  document's `SelectionLanguage` is `XSLPattern` (the default of the MSXML
  3, 2 and version-independent classes), each element name test becomes a
  test of the node's qualified name: `UX` is any element written `UX`,
  whatever its default namespace, `x:UX` one written with the prefix `x`,
  `x:*` any element written with `x:`, and a prefixed attribute matches as
  written too.  A prefix declared in `SelectionNamespaces` still means its
  namespace.  The query is tokenised by XPath's own lexical rules, so
  functions, node types, axes, the `and`/`or`/`div`/`mod` operators,
  literals and variables are left alone.  XPath (MSXML 6's only language,
  or after `setProperty("SelectionLanguage", "XPath")`) is unchanged.  Not
  yet: XSL Patterns' own operators and methods (`$eq$`, `$and$`, `end()`,
  `index()`), which no installer seen so far uses.
- **The self-test** `msxmltest` (core 147) checks both: Burn's queries on
  an MSXML 3 document, prefixes as written, `SelectionNamespaces`, names
  that look like operators or node types, and the same queries matching
  nothing under XPath and on an MSXML 6 document (130 checks, 64- and
  32-bit).

**Where the Visual C++ Redistributable stops now**: Burn reads its whole
manifest, detects, plans, loads its bootstrapper application and starts
its elevated engine, which fails 0x80070005 (access denied) creating its
cache folder `C:\ProgramData\Package Cache\{bundle id}\` ("Failed to
create cache directory"), so it registers nothing and exits with code 5.
Creating that folder from `cmd` works, so the gap is in the secured
folder Burn makes there (its own ACL) as the elevated engine.
