# Windows.UI.Xaml on NovaOS

UWP apps (Windows Calculator, the Store's apps, many DirectX games packaged
for the Store) and C++/WinRT or C# programs that host XAML draw their
windows with the Windows Runtime's XAML framework, `Windows.UI.Xaml`.
This page is the plan for running that framework on NovaOS: what
Windows' own stack looks like, which parts NovaOS takes from Microsoft's
MIT-licensed source, which parts NovaOS has to provide because they are
the operating system, and the steps (roadmap Phase 32).

## The stack on Windows

From the bottom up:

| Layer | Windows | On NovaOS |
|---|---|---|
| Windows Runtime core | `combase.dll`: `HSTRING`, `RoGetActivationFactory`, `RoActivateInstance`, the registry's `ActivatableClassId` keys, `.winmd` metadata | `ole32.dll` (`userland/ole32/winrt.c`) |
| App model | `twinapi.appcore.dll` (`CoreApplication`, views), `Windows.UI.dll` (`CoreWindow`, `CoreDispatcher`) | `twinapi.appcore.dll` (`userland/twinapi_appcore`), step 32.1 |
| Composition | `Windows.UI.Composition` (the system compositor's WinRT API, over DirectComposition and Direct3D 11) | `dcomp.dll` (`userland/dcomp/composition.c`), step 32.2 |
| XAML | `Windows.UI.Xaml.dll` (system XAML) and WinUI 2 (`Microsoft.UI.Xaml.dll`, controls on top of it) | Microsoft's MIT XAML source, built for NovaOS, steps 32.3 on |
| Packages | AppX/MSIX deployment, `resources.pri` (MRT), the app container | `appx.exe`, `MRM.dll` (MRT Core, MIT), package identity in kernel32; bundles, frameworks, signatures and the app container to write |

## Where the XAML framework comes from

Dean's standing rule is to reuse what Microsoft publishes under MIT
rather than write it again. The system `Windows.UI.Xaml.dll` itself is not
published, but its successor is: WinUI 3 is the same XAML framework
"lifted" out of Windows into `Microsoft.UI.Xaml`, and its full source,
the `dxaml` tree (the XAML core, parser, layout, rendering, controls and
their templates), is MIT in
[github.com/microsoft/microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml),
together with WinUI 2 (the `controls` tree). C++/WinRT
([github.com/microsoft/cppwinrt](https://github.com/microsoft/cppwinrt))
and WIL ([github.com/microsoft/wil](https://github.com/microsoft/wil)),
which that source is written against, are MIT too.

So NovaOS builds `dxaml` (unchanged where possible, as with WDF and
CalcManager) under both names:

- as **`Windows.UI.Xaml.dll`**, with its types registered under the
  `Windows.UI.Xaml` namespace, for UWP apps and WinUI 2 (the type tables
  and projections are generated from the framework's IDL and metadata, so
  the namespace is a code-generation choice; WinUI 3 began as this same
  code under `Windows.UI.Xaml`);
- later as **`Microsoft.UI.Xaml.dll`**, for Windows App SDK apps.

What the `dxaml` build needs that Microsoft does not publish is the
"lifted" Interactive Experiences layer it links against
(`Microsoft.UI.Composition`, `Microsoft.UI.Input`, `CoreMessaging` and
`Microsoft.Internal.FrameworkUdk`, shipped closed in the Windows App
SDK). On Windows the system XAML uses the in-box equivalents instead:
`Windows.UI.Composition`, `CoreWindow` and the system's input. Those are
operating-system components, so NovaOS writes them (steps 32.1 and 32.2)
and points the XAML build at them. NovaOS does not ship Microsoft's
closed binaries.

The build is clang in MSVC mode (as for the rest of NovaOS's C++).  The
headers its IDL would give through MIDL (a closed tool) come from
`tools/winrt/abigen.py` instead (step 32.3's second part, below): it
reads the framework's MIDL 3 IDL and Windows Runtime metadata and writes
the same C++ ABI headers, at build time, from inputs the repository
vendors.

## The steps (Phase 32)

1. **32.1 WinRT activation from the registry; CoreApplication and
   CoreWindow.** `RoGetActivationFactory` loads the in-process server the
   registry names (`HKLM\SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId\<class>`,
   `DllPath`, `ActivationType` 0) and calls its `DllGetActivationFactory`,
   as combase does. `twinapi.appcore.dll` serves
   `Windows.ApplicationModel.Core.CoreApplication` (`Run` drives an
   `IFrameworkView`: `Initialize`, `SetWindow`, `Load`, the launch's
   `Activated`, `Run`, `Uninitialize`; `Exit`; the main view) and
   `Windows.UI.Core.CoreWindow` (a top-level window with its
   `ICoreWindowInterop` HWND, `Activate`, `Close`, bounds, key states,
   pointer capture and cursor; events `Activated`, `CharacterReceived`,
   `Closed`, `KeyDown`, `KeyUp`, `SizeChanged`, `VisibilityChanged`; the
   `CoreDispatcher` with `ProcessEvents` and `RunAsync`). DXGI's
   `CreateSwapChainForCoreWindow` presents into it, so a DirectX UWP
   app's frame loop runs. Test: `coreapptest`.
2. **32.2 Windows.UI.Composition.** `Compositor`, container and sprite
   visuals, colour and surface brushes, `CompositionGraphicsDevice`
   drawing surfaces over Direct2D, the desktop and CoreWindow targets,
   and the interop interfaces XAML uses, on NovaOS's DirectComposition.
   CoreWindow's pointer events (`PointerPoint`) join here.  Done as
   follows: `dcomp.dll` serves `Windows.UI.Composition.Compositor` (as
   Windows' does) with container and sprite visuals (offset, size,
   anchor and centre points, scale, Z rotation, the transform matrix's
   2-D part, opacity, visibility, inset clips, the visual collection),
   colour and surface brushes (all four stretches, alignment ratios),
   targets on a window (`ICompositorDesktopInterop`) or on the thread's
   CoreWindow (`CreateTargetForCurrentView`), graphics devices on a
   Direct3D 11 device and their drawing surfaces
   (`ICompositorInterop`, `ICompositionDrawingSurfaceInterop`: the
   surface's texture, read back at `EndDraw` as DirectComposition's own
   surfaces are) and surfaces showing a composition swap chain.  A thread
   of the compositor's draws each target's tree in software after every
   change and copies it into the window with the GDI.  Not yet:
   animations (accepted, not run), expression animations, property sets,
   effects, batches, 3-D transforms, clips other than insets and bilinear
   sampling (drawing surfaces on an `ID2D1Device` came with 32.3's first
   part).  `twinapi.appcore`'s CoreWindow raises
   `PointerEntered`, `PointerMoved`, `PointerPressed`, `PointerReleased`,
   `PointerWheelChanged`, `PointerExited` and `PointerCaptureLost` from
   the mouse's messages with a `PointerPoint` (pointer 1, a mouse
   `PointerDevice`) and its `PointerPointProperties`.  Test: `comptest`,
   and `coreapptest`'s pointer and CoreWindow-target checks.
   - *The interfaces XAML's render tree asks for (Phase 32 push).*
     XAML's source holds the compositor as `Microsoft.UI.Composition`
     (`NamespaceAliases.h`'s `WUComp`), and `DCompTreeHost` asks it for
     `ICompositor2`, `5`, `6` and `ICompositorSwapChainInterop`.  The two
     namespaces' interfaces have the same members in the same order,
     only their IIDs differ, but for `ICompositor` (no
     `CreateTargetForCurrentView` in the Windows App SDK's),
     `ICompositionObject` (no `Dispatcher`) and the `DispatcherQueue`
     of `ICompositionObject3` and `ICompositor7`; so each `dcomp.dll`
     object answers both sets of IIDs (`composition_iids.h`, from
     `third_party/winmd`), with a vtable of its own where they differ, and
     `Microsoft.UI.Composition.Compositor` is registered to `dcomp.dll`
     too.  Added: `ICompositor2` to `8` (animation groups, step easing,
     `Comment`, `GlobalPlaybackRate`, Boolean key frames, rectangle clips,
     `DispatcherQueue`; lights, shadows, shapes, gradient brushes and the
     rest still `E_NOTIMPL`), `IVisual2` to `4` (`RelativeOffsetAdjustment`
     and `RelativeSizeAdjustment` drawn), `ICompositionObject2` to `5`,
     each object's `Properties` and `CreatePropertySet` (property sets of
     every value type), key frame animations of scalars, vectors, colours,
     quaternions and Booleans run on the compositor's thread (delay,
     duration, iterations, direction, stop and delay behaviours, the
     expression key frames `this.StartingValue` and `this.FinalValue`),
     animation groups started through their animations' `Target`,
     expression animations (accepted and stopped, their expressions not
     yet evaluated), scoped and commit batches whose `Completed` is raised
     on the making thread's `DispatcherQueue`, linear, cubic Bezier,
     step, sine, circle and power easing (`CompositionEasingFunction`'s
     statics too; back, bounce, elastic and exponential ease linearly) and
     rectangle clips (square corners).  Test: `comptest`.
3. **32.3 The XAML core builds.** `dxaml`'s core, parser and metadata
   built as `Windows.UI.Xaml.dll` against 32.1 and 32.2;
   `Windows.UI.Xaml.Application.Start` runs and `XamlReader.Load` parses a
   page into its object tree.  Several pull requests:
   - *Direct2D device contexts (done).*  XAML draws its text and shapes
     with Direct2D 1.1: it makes an `ID2D1Factory1` (and takes its
     `ID2D1Multithread`), an `ID2D1Device` on its Direct3D 11 device and
     that device's contexts, and draws into DirectComposition surfaces
     made through an `IDCompositionSurfaceFactory` on the `ID2D1Device`,
     whose `BeginDraw` hands it a device context (`dxaml/xcp/plat/win/desktop`:
     `D3D11DeviceInstance.cpp`, `DCompSurface.cpp`, `D2DAcceleratedRT.cpp`).
     `d2d1.dll` now has `ID2D1Factory1`, `ID2D1Device`,
     `ID2D1DeviceContext` (its target a bitmap of its own or of a DXGI
     surface, read from the texture at `BeginDraw` and written back at
     `Flush` and `EndDraw`, so any Direct3D 11 device works),
     `ID2D1Bitmap1` and the other 1.1 interfaces it hands out;
     DirectComposition and Windows.UI.Composition take an `ID2D1Device`
     as their rendering device and give out device contexts from
     `BeginDraw`.  Test: `d2dctxtest`, and `comptest`'s Direct2D drawing
     surface.  Not yet: effects, command lists, image brushes, colour
     contexts, `ID2D1DeviceContext1` and later (XAML asks for
     `ID2D1DeviceContext4`/`5` only for colour glyphs and SVG).
   - *The headers MIDL would make (done).*  `dxaml` is written against
     the C++ ABI headers MIDL makes from its IDL
     (`dxaml/xcp/dxaml/idl`, MIDL 3) and those of the Windows SDK and
     the Windows App SDK.  `tools/winrt/abigen.py` writes them without
     MIDL: `winmd.py` reads Windows Runtime metadata (`Windows.winmd` and
     the lifted `Microsoft.UI*` `.winmd` files from windows-rs, MIT,
     `third_party/winmd`), `idl.py` reads the MIDL 3 IDL and synthesizes
     the interfaces MIDL makes for a runtime class (`IFoo` for its
     instance members, `IFooStatics`, `IFooFactory` with a composable
     class's `baseInterface`/`innerInterface`, `IFooOverrides`,
     `IFooProtected`, `IFoo2`... for versioned member blocks, the names
     `[interface_name]` and its kin give), and the writer emits one
     `namespace.h` per namespace (interfaces in vtable order with
     `__declspec(uuid)`, delegates as `IFooHandler`, the generic
     instances the namespace's types use as `template <>`
     specializations with the IIDs Windows' pinterface rule gives,
     `__FIVector_1_...` names, `RuntimeClass_`/`InterfaceName_` strings)
     and a `namespace.values.h` (enums with MIDL's `Name_Value`
     constants, structs, the generic interfaces' `_impl` templates).
     MIDL 3 invents the IIDs of the interfaces it synthesizes; NovaOS
     takes them by name from Microsoft's published
     `Microsoft.UI.Xaml.winmd`, so they are the IIDs Windows App SDK
     apps use (interfaces only in the IDL, such as the private ones, get
     name-based IIDs of NovaOS's).  `abigen.py --check` confirms that the
     IDL's 2136 public types match that metadata in names, member order
     and signatures.  Features `FeatureFlags.h` marks `AlwaysDisabled`
     are left out, as MIDL does.  The userland build generates the headers
     for any program or DLL whose manifest asks (`"winrt_abi": true`).
     Test: `winrtabitest` (published IIDs, and the Compositor driven
     through the generated interfaces).
   - *The `dxaml` toolchain and the core's lowest layer (done).*
     `userland/windows_ui_xaml/build.py` compiles Microsoft's sources as
     Microsoft does: clang in MSVC mode, C++17 without C++ exceptions, over
     Microsoft's STL and MinGW-w64's Windows headers (as `msvcp140.dll`'s
     build), with each component's sources, include directories and
     definitions read from its MSBuild project (`vcxproj.py`) and
     `components/pch/precomp.h` forced in.  What the sources need beyond
     that: WIL (MIT, vendored in `third_party/wil`); in
     `userland/windows_ui_xaml/inc` NovaOS's own WRL (the Windows SDK's
     Windows Runtime C++ template library: `ComPtr`, `WeakRef`,
     `RuntimeClass` with `Implements`, `CloakedIid`, `ChainInterfaces`,
     `MixIn`, `ComposableBase` and `FtmBase`, `Make`,
     `MakeAndInitialize`, `Callback`, `EventSource`, `HString`,
     `HStringReference`, `Module`, the handle wrappers), `weakreference.h`,
     `xamlom.h` (XAML diagnostics, from its public documentation), the
     SAL annotations, Windows Runtime `HRESULT`s and `MIDL_INTERFACE`
     with its `uuid` that MinGW-w64 lacks, and TraceLogging compiled out;
     ETW compiled out too, with the events header `mc.exe` would make from
     XAML's manifest written by `etwgen.py`; `abigen.py` now also writes
     MIDL's per-IDL-file headers (`Microsoft.UI.Xaml.controls.controls.h`,
     `Microsoft.UI.Xaml.private.h`); and a directory of one-line headers
     maps each include spelling the sources use to the file a
     case-insensitive disk would find.  The components below
     `CDependencyObject` build into `xaml_components.lib`: `base`,
     `xstring`, `criticalsection`, `dependencyLocator`,
     `runtimeEnabledFeatures`, `com`, `threading`, `staticpal` and
     `qualifiers` (their sources joined `third_party/microsoft-ui-xaml`).
     A program says `"xaml": true` to be built the same way and linked
     with it.  Test: `xamlbasetest`.
   - *What is closed.*  The open-source tree is not all of XAML: 75
     headers it includes come from closed Windows App SDK pieces, chiefly
     FrameworkUdk (`InputSiteHelpers.h`, which `CDependencyObject.h`
     includes, so every component above the lowest layer stops there;
     `CoreWindow.h`, `Theming.h`, `MUIUDK.h`), the lifted input and
     composition interop headers (`microsoft.ui.composition.interop.h`,
     `microsoft.ui.composition.experimental.h`) and the PTLS text engine
     (`ptls-min.h`, `XamlTextHost.h`); plus SDK headers MinGW-w64 lacks
     (`d2d1_3.h`, `async.h`, `atlbase.h`, `rometadataresolution.h`).
     NovaOS writes these as its own, from how the open sources call them
     and public documentation.
   - *The closed headers, the metadata and `CDependencyObject` (done so
     far).*  `userland/windows_ui_xaml/inc` now has the stand-ins the
     core's lower layers include: FrameworkUdk's `InputSiteHelpers.h`
     (an island's input site and its window), `InputHelpers.h`,
     `ContentExternalLinkHelper.h`, `TextInputProducerHelper.h` and
     `FrameworkUdk/FrameworkInputView.h`; the lifted IXP's
     `microsoft.ui.composition.interop.h` (`ICompositorSwapChainInterop`),
     `microsoft.ui.composition.experimental.h` and
     `microsoft.ui.input.inputpretranslatesource.interop.h`;
     `Microsoft.DirectManipulation.h` (the Windows App SDK's additions to
     DirectManipulation); and from the Windows SDK, WRL's `async.h`
     (`AsyncBase`), `rometadataresolution.h`, a `d2d1_3.h` that so far
     gives Direct2D 1.1, the UI Automation interfaces MinGW-w64's
     `uiautomationcore.h` lacks (their IIDs from Windows' Win32
     metadata), `GUID_NULL` and constexpr enum-flag operators.  Lifted
     types newer than windows-rs's `Microsoft.UI.winmd`
     (`IInputPointerSource2`, `PopupWindowSiteBridge`) come from NovaOS's
     own IDL (`userland/windows_ui_xaml/idl`), which also declares the
     `IReference<T>` instances XAML's metadata names; `AsyncStatus` is a
     scoped enum, as in the SDK.  With them the type metadata (`metadata`,
     `DynamicMetadataStorage`: every built-in type, property and
     namespace) builds and links, and `CDependencyObject`'s property
     system (`DependencyObject`) compiles into `xaml_components.lib`.
     `xamlbasetest` looks types and properties up by index and by name;
     the metadata's two calls into the layers above (custom types'
     providers, class constructors) are Microsoft's own stubs from its
     isolated unit tests until those layers build.
   - *The collections (done so far).*  `Collection` builds and links
     with `CDependencyObject` and the value types under it: `CValue`
     (whose `CValueStores.cpp` NovaOS wraps with explicit member
     instantiations, `userland/windows_ui_xaml/src`, because clang does
     not instantiate members defined after a `template struct`), the
     flyweight values, the primitive dependency objects, `math` (with
     `pal/common`'s geometry helpers) and `theming`.  The composition
     tree's `CompHelper/CompositionHelper.h` and
     `CompositionVisualSurfaceHelper.h` are declare-only stand-ins until
     the composition tree builds; `CoreWindowSiteBridge` joined NovaOS's
     lifted IDL.  Microsoft's release build leaves `DBG` undefined, and
     so does NovaOS's now.  The layers above (the core's `CCoreServices`,
     `CUIElement`, the resources, the visual tree) are Microsoft's own
     isolated unit-test stubs and metadata mocks, built with
     `__XAML_UNITTESTS__` as `xaml_teststubs.lib` for `xamlbasetest`
     only, plus a few NovaOS stand-ins (`src/core_stubs.cpp`) for what
     those stubs leave out.  `xamlbasetest` fills, searches and empties
     a `DependencyObjectCollection`, as Microsoft's own collection tests
     do, and walks a `CollectionMoveView`.  NovaOS gained what the
     sources needed below them: `thread_local` destructors (`__tlregdtor`
     in `userland/lib/tlregdtr.c`, linked only into C++ programs that have one), the UCRT's `_MAX_*TOSTR_*_COUNT` sizes,
     and `windowsnumerics.h`'s `invert` (MinGW-w64 declares it only).
   - *The resources and the parser (done so far).*  The `resources`,
     `deferral`, `legacy`, `colors` and `winuri` components join
     `xaml_components.lib`, and the markup parser (`core/Parser`, built
     as Microsoft builds it: the core's include path read from its
     `sources.props` and the `.props` files it imports, its own
     precompiled header, C++ exceptions on) with the `pal/common`
     pieces it calls (`MemoryStream`, `xstrlen`, the resource helpers;
     `src/pal.cpp`).  The parser reads XML through XmlLite, so NovaOS
     now has `xmllite.dll` (`userland/xmllite`): `IXmlReader` over
     libxml2's `xmlTextReader` (third_party/libxml2, MIT), each node and
     its attributes, namespace declarations among them, copied out as
     UTF-16 until the next `Read`, with the encodings XmlLite infers.
     WinURI's `CWinUriFactory` needed urlmon's `IUriBuilder`, now in
     `userland/urlmon`.  Stand-ins for `windows.graphics.interop.h`,
     `d2d1svg.h` (declared, empty) and a `tchar.h` that lets WinURI
     include `strsafe.h` after it.  The heap's `HeapSize` now gives the
     size asked for, as Windows' does, not its size class
     (`CreateStreamOnHGlobal` streams were as long as the class).
     `xamlparsetest` (`"xaml": "parser"`, on Microsoft's mock parser
     services from its isolated parser unit tests) reads XML with
     XmlLite, parses XAML into the node stream (namespaces, objects,
     implicit collections, content properties, `x:String`,
     `mc:Ignorable`, `{StaticResource}`, a `ResourceDictionary` with
     `x:Key`), reports bad XML and bad markup extensions, and looks up
     resource keys in a dictionary's map and the theme walk's cache.
   - *The resource dictionary and the object writer (done so far).*
     Two more of the core's libraries in `CORE_LIBRARIES`: the parser's
     object writer runtime (`core/Parser/runtime`: `XamlNativeRuntime`,
     which creates the core's objects through the type table's
     constructors and sets their properties, collections and dictionary
     entries; the managed runtime; the binary object writer) and the
     elements' exception-built library (`elements.except.vcxproj`:
     `Resources.cpp`'s `CResourceDictionary`, `ThemeResourceExtension`, `VisualTransition`,
     `XamlPredicate`), each with its project's precompiled header, and
     two small components they call (`simple`'s simple properties and
     `namescope`'s name scopes) and the elements' `DeferredKeys.cpp`.
     What they call further up the core (`CCoreServices`' resource
     loading and theming, templates, bindings, the visual state manager,
     the framework callbacks) has stand-ins in `src/elements_stubs.cpp`
     that report it absent; two of Microsoft's unit-test stubs answer
     where the core's own code is wanted (`AddPeerReferenceToItem` says
     `E_NOTIMPL`, which fails every collection add, and no resource
     lookup logger), so `build.py`'s `STUB_RENAMES` renames them out of
     their stub files and the core's bodies stand in.
     Stand-ins for the closed or missing headers they include:
     `WinUIEdit.h` (the Windows App SDK's RichEdit, here the SDK's
     RichEdit, text services and TOM headers), `mfmediaengine.h`,
     `MFSharingEngine.h` and `castinginterop.h` (the media engine
     callback, the sharing engine factory and the casting interfaces,
     declared from their documentation) and
     `CompHelper/CompositionSurfaceHelper.h` (declared as
     `DCompSurface.cpp` calls it).  The SDK's namespace headers include
     the namespaces they import; `abigen.py`'s only declare them, which
     compiles each XAML source in about 60% of the time, so the elements
     are given the two namespaces whose interfaces their headers use
     whole (`Windows.ApplicationModel`, `Windows.Devices.Input`) first.
     `xamlloadtest` (`"xaml": "elements"`) loads XAML text into objects:
     XmlLite, the parser and the object writer make a live
     `CResourceDictionary` of `x:String`, `x:Double`, `x:Int32`,
     `x:Boolean` and `Color` resources, and refuse malformed markup; it
     also adds, finds, refuses and removes keys directly.  Until
     `CCoreServices` builds, its core is Microsoft's mock parser services
     laid where a `CCoreServices` holds them, with the few fields the
     dictionary reads set as the core's constructor sets them (through
     `DxamlCoreTestHooks`, the core's friend; theming over Microsoft's
     `TestThemingInterop`), and its type table constructs the six types
     it uses.
   - *Part 8: merged and theme dictionaries.*  `MergedDictionaries` and
     `ThemeDictionaries` are field-backed properties: the object writer
     reads and sets the field at the offset the core's generated metadata
     (`core/metadata/DynamicMetadata.g.cpp`,
     `c_aDependencyPropertyRuntimeData`) gives the property.  That table
     has a row for every property of every element and names every
     element class, so it cannot compile before the elements do.
     `src/property_data.cpp` keeps Microsoft's rows for the classes that
     build (the `ResourceDictionary`'s three, so far), found by property
     index, with `ReflectionAPI.cpp`'s getters over them (and its
     `CClassInfo::GetCoreConstructor`, from the type table) and
     `DependencyProperty.cpp`'s `CreateDefaultValueObject` (a property's
     default object made by its type's constructor: the merged
     dictionaries' collection, and the theme dictionaries' dictionary,
     marked as one); `build.py` renames the metadata mocks' versions out
     of the way.  A row is added as its class builds, until the whole
     table compiles with the core.  `xamlloadtest` loads a dictionary
     with two merged dictionaries (the last one wins a shared key) and
     `Light` and `Dark` theme dictionaries, and looks keys up through
     them.
   - *Part 9: the core services.*  `CCoreServices`
     (`core/dll/xcpcore.cpp` and `xcpcore_namescope.cpp`, with the two
     small providers it holds from `core/hw`) compiles with the core's own
     project, `xcpcore.vcxproj` (`build.py`'s `CORE_SOURCES` names the
     sources built so far), into `xaml_core.lib`, which programs that say
     `"xaml": "core"` link; their stubs are Microsoft's and NovaOS's less
     those that stand in for the core services themselves
     (`CoreServices_Stub.cpp`, `src/core_services_stubs.cpp`, the error
     and platform services: `xaml_corestubs.lib`).  It needed 65 more of
     Microsoft's headers and one more closed one
     (`FrameworkUdk/CoreWindowIntegrationInterface.h`, the CoreWindow's
     position listener).  What it calls in the rest of the core (about
     340 functions: rendering and composition, input, layout, text,
     media, the element tree) is worked out at each build, the libraries'
     undefined names less their defined ones (`llvm-nm`), and each gets a
     generated thunk made the function's alternate name (`lld-link
     /alternatename`, so a real definition always wins): it reports its
     name once ("XAML core: function not built yet") and returns what
     its return type says is "not implemented" (`E_NOTIMPL`, nothing,
     zero, null; `llvm-undname` reads the type); a constructor or a
     function returning an object by value stops the program, naming
     itself.  `xamlloadtest` makes the core with its own constructor and
     loads XAML through `CCoreServices::ParseXaml`, the path
     `XamlReader.Load` takes, so the core's own `IParserCoreServices`
     and schema context serve the parser; what `CCoreServices::Create`
     sets up that does not build yet (the platform layer's queues, the
     text core, the media queue) is left out, and the few fields the
     dictionary reads are set as `Create` sets them (through
     `DxamlCoreTestHooks`).  The framework would load the core's theme
     resources on first lookup; the test gives it a small dictionary
     of its own.
   - *Part 10: the core services made as the framework makes them.*
     `CCoreServices::Create` runs for real: the program hands the core
     the platform layer's services (`gps`) through
     `ObtainPlatformServices` and `ObtainCoreServices`, as `DXamlCore`
     does.  The platform layer is Microsoft's `CWindowsServices`
     (`plat/win/browserdesktop`: `xcpwin.cpp`, `PlatformServices.cpp`,
     the thread pool's work items and the one global instance,
     compiled with its own project and precompiled header), which gives
     the core its queues, work item factory and security token.  What
     `Create` makes builds too: the media queue manager, the render
     target bitmap manager, the geometry builder (the elements'
     `geometry.cpp`), the text core `CTextCore` with its Windows half
     `WinTextCore`, and the system theming (`components/theminginterop`,
     reading the light or dark theme from the registry).  The text core's
     own precompiled header includes the closed Page/Table Layout
     Services' header, so `TextCore.cpp` is compiled with the core's
     sources and a precompiled header of NovaOS's
     (`src/core_precomp.h`: the core's and the four text headers
     `CTextCore` names).  New stand-ins for closed or missing headers:
     ATL's `atlbase.h` (declarations only: the input pane handler's base
     class, which nothing built instantiates), `atlhost.h`,
     `shlobj_core.h`, the WPP trace header `xcpwin.tmh` (tracing compiled
     out), FrameworkUdk's `DirectManipulationHelper.h`, and `d2d1_1.h`,
     which gives MinGW-w64's Direct2D 1.1 interfaces their IIDs as
     `__declspec(uuid)` (MinGW-w64's own `__CRT_UUID_DECL` is empty where
     the compiler has `__uuidof`).  `xamlloadtest` makes its core this
     way (no test hooks), with the system theme fixed at Dark through the
     theming's own test hook, and everything else as in part 9.
   - *Part 11: the framework's entry points.*  `dxaml/lib`'s first
     sources build on the core, as their own project
     (`winrtfoundation/wrtdxamlfoundation.vcxproj`): `DXamlCore` (the
     per-thread framework core whose `InitializeInstance` calls
     `ObtainCoreServices`), its instance storage, `DXamlServices`,
     `comTemplateLibrary`, `XamlReader` and `FrameworkApplication`, with
     their generated classes (`XamlReader.g.cpp`,
     `FrameworkApplication.g.cpp`).  The generated ABI headers grew what
     these sources name: a disabled feature's members become their own
     interface (`IXxxFeature_...`), unsealed classes always get their
     factory, generic instances are closed over the interfaces they
     require, and collection delegates keep their Windows names.  New
     stand-ins: the restricted error info (`roerrorapi.h`), WRL's
     `WrlSealed` and activatable class macros (activation goes through
     the framework's own factories), `ICoreWindowInterop`, the
     dispatcher's interop header, FrameworkUdk's back button, debug tool
     and visual diagnostics port headers, Microsoft's TIP telemetry
     (compiled out), `shobjidl_core.h` and WIL's app model header.
     NovaOS gains MSVC's dynamic `thread_local` initialisation
     (`__dyn_tls_init`, which `DXamlCore`'s per-thread storage uses),
     `AppPolicyGetWindowingModel` and `ImmDisableLegacyIME`.
     `xamlloadtest` makes `XamlReader`'s activation factory and calls
     `XamlReader.Load` on a thread with no framework core, which answers
     `RPC_E_WRONG_THREAD` as Windows does.
   - *Part 12: the control layer and the DispatcherQueue.*  Every XAML
     thread runs on a `DispatcherQueue`, so NovaOS gains
     `CoreMessaging.dll`: the `DispatcherQueue`, its controller and
     timers for both `Microsoft.UI.Dispatching` (the Windows App SDK's)
     and `Windows.System`, one queue per thread over a message-only
     window, with high, normal and low priority work, the shutdown
     events and their deferrals, `RunEventLoop`, dedicated threads and
     `CreateDispatcherQueueController` (test `dispatchqtest`).  ole32
     gains the restricted error info (`RoOriginateError` keeps an
     `IRestrictedErrorInfo` per thread, `GetRestrictedErrorInfo`,
     `RoGetErrorReportingFlags`).  Built from Microsoft's sources: the
     error helper (`ErrorHelper.cpp`), `CJupiterControl`, `CControlBase`,
     the XAML window's dispatcher `CXcpDispatcher` with its pending
     messages, and the browser host `CXcpBrowserHost`, on new stand-ins
     for FrameworkUdk's `FinalUnhandledErrorDetected` and
     `VisualDiagnosticsPort` functions (`src/framework_udk.cpp`).
     `xamlloadtest` makes a `CXcpDispatcher` on a thread's
     `DispatcherQueue` and has it run its tick, then calls
     `CJupiterControl::Create`, which gets through the platform layer,
     the dispatcher and the core services and stops at the rendering
     seam (`CommonBrowserHost::CreateRenderTarget`: the graphics device
     manager, the compositor and UI thread schedulers and the window
     render target), not yet built, with `E_NOTIMPL`.
   - *Part 13: the rendering seam.*  `CJupiterControl::Create` now
     gets through `CommonBrowserHost::CreateRenderTarget`, built from
     Microsoft's sources: the graphics device manager
     (`WindowsGraphicsDeviceManager`, with `DCompTreeHost`,
     `OfferTracker`, `ProjectedShadowManager` and the refresh rate
     info), the compositor scheduler with its refresh-aligned clock, the
     UI thread scheduler, and the window render target
     (`CWindowRenderTarget`: the hardware walk `HWWalk` with its surface
     cache and texture manager, and `CompositorTreeHost`), plus what it
     calls on the way: the error service, the downloader and its async
     request manager, the value store, `WUCBrushManager` and the
     hardware render commands.  The device manager makes its Direct3D 11
     device (`CD3D11Device`, `CD3D11DeviceInstance`) and Direct2D device
     (`CD2DFactory`) on a thread of its own and draws a first line of
     text there with DirectWrite.  Without DXVK that device is NovaOS's
     software device (`userland/d3d11/soft.c`), which gains
     `IDXGIDevice2`/`3` (offer, reclaim and trim, which have nothing to
     do in memory), `ID3D10Multithread` (a critical section) and
     `ID3D11Device4` (device-removed events that never fire); ole32
     gains combase's `RoFailFastWithErrorContextInternal2`, which XAML's
     fail-fast paths call.  `plat/win/desktop`'s precompiled header names
     closed headers (MUI, print preview, the windowless RichEdit host),
     so NovaOS's own `src/platdesktop/precomp.h` stands in for it.
     The one stand-in this path left,
     `CJupiterWindow::GetJupiterWindowPhysicalSize` (the window class
     needs the closed `FrameworkUdk/CoreWindowIntegration.h`), is now
     NovaOS's own (below).  `xamlloadtest` checks that
     `CJupiterControl::Create` succeeds with its render target.
   - *Phase 32 push, `DXamlCore`'s start-up.*  Built from Microsoft's
     sources: the XAML dispatcher `DispatcherImpl` (`dispatcher.cpp`),
     the default control styles' cache `DefaultStyles`, the `Window`
     (`Window_Partial.cpp` and its generated class) and the framework's
     peer base class `DirectUI::DependencyObject`.  The isolated tests'
     `DependencyObject_Stub.cpp` defines two of the peer base's
     members; programs on the core link it with those renamed out
     (`CORE_STUB_RENAMES` in `build.py`).  Of the framework's window,
     `CJupiterWindow`, whose source includes headers Microsoft does not
     publish, NovaOS writes the one member the control layer calls on a
     resize, its client size in physical pixels
     (`src/jupiter_window.cpp`).  `xamlloadtest` makes a
     `DispatcherImpl` (unconnected, it is no thread's and refuses work)
     and a `DefaultStyles`.  A `Window` is made only on an initialized
     `DXamlCore` (its constructor reads the thread's core), and it
     fails fast unless the `EnableUWPWindow` feature is on or the core
     was initialized for islands.
   - *The value boxer (Phase 32 push).*  What `XamlReader.Load` hands
     its result back through.  NovaOS gains `wintypes.dll` with
     `Windows.Foundation.PropertyValue`, the Windows Runtime's boxes
     (every scalar and array type of `PropertyType`; `IPropertyValue`'s
     numeric getters convert in range, as Windows' do; each box answers
     its own `IReference<T>` or `IReferenceArray<T>`, whose IIDs are the
     parameterized interfaces' worked out from their signatures; test
     `propvaltest`).  Built from Microsoft's sources: the value boxer
     (`components/valueboxer`: `CValueBoxer`, `PropertyValue`'s wrappers)
     and the lifetime library (`components/lifetime`:
     `ReferenceTrackerManager`, the weak reference sources, tracker
     targets), each as its own core library with Microsoft's flags (they
     call into the framework, so programs without it keep Microsoft's
     stubs); the framework's `StaticStore` (the `PropertyValue` statics
     and cached boxes) and `UIAffinityReleaseQueue`; and the core's
     error service (`core/error`), deployment (`core/packaging`) and
     entry points (`core/dll/CoreImports.cpp`).  The ABI headers gain
     the `IVectorView` instances of numbers XAML's own declarations add.
     `xamlloadtest` starts the framework on a thread as the XAML compiler
     does (`DXamlCore::Initialize(InitializationType::Xbf)`, which needs
     no window or rendering; the test makes the framework's per-thread
     slot first, as its `DllMain` does), parses an `x:String` with that
     thread's core and has `CValueBoxer` unbox it, and numbers and
     booleans, into `PropertyValue` boxes, and box them back (given
     the target's type, as a property's setter passes it: finding an
     untyped box's type is the reflection API's, `core/metadata`, not
     built yet).
     `XamlReader.Load` itself parses through the browser host
     (`Host_CreateFromXaml`), which the full start-up now makes past the
     rendering seam, so it runs end to end once the element classes and
     their framework peers build.
   - *`XamlReader.Load` end to end (Phase 32 push).*  Built from
     Microsoft's sources: the element classes (`core/core/elements` and
     `elements2`, less the few whose sources include closed headers), the
     text core's `TextBlock` subset (`core/text`, with NovaOS's own
     precompiled header in place of the project's, which pulls in the
     closed line services), the real type table (`core/metadata/dynamic`,
     replacing the isolated tests' mocks and NovaOS's empty
     activation table), the core's event manager, `components/elements`
     (`CUIElement`, `CFrameworkElement`), and the framework peers of
     `UIElement`, `FrameworkElement`, `Panel`, `Grid`, `TextBlock`,
     `Control`, `ContentControl`, `ButtonBase`, `Button`, `Border`,
     `ContentPresenter`, `ResourceDictionary`, `Style`, `Setter`, the
     templates and their event args, with the framework's callbacks the
     core and the parser make (`FxCallbacks.cpp`,
     `XamlParserCallbacks.cpp`), type activation (`ActivationAPI.cpp`)
     and the framework's qualified objects. The stubs of what now builds
     leave `xaml_corestubs.lib` (`CORE_SERVICES_STUBS`).  NovaOS adds
     only what no published header or metadata gives: the Windows SDK's
     name for `CoreWindow.Closed`'s handler and a stand-in for
     `GamepadKeyRoutingConfiguration` (contract 19, never activated here)
     in `src/framework_first.h`; the COM IIDs uuid.lib holds
     (`src/uuids.cpp`, since an IID the link leaves undefined would
     otherwise alias generated code); `ID2D1DeviceContext5`'s declaration;
     and `abigen.py` now puts flag enums' operators in the enum's
     namespace, emits the generic instances runtime classes implement and
     defines the universal API contract's version per header, as MIDL
     does.  `xamlloadtest` starts the framework as `Application.Start`
     does on a thread with a `DispatcherQueue`
     (`DXamlCore::Initialize(InitializationType::IslandsOnly)`), then
     `XamlReader.Load` of a `Grid` holding a `TextBlock` with an
     `x:Name` and a `Button` hands back the framework objects: the Grid's
     two children through `IPanel`, the text and name, the Button's
     content through `IContentControl`, and the TextBlock's core object
     parented to the Grid's (`FrameworkElement.Parent` stays null outside
     a live tree, as on Windows).  The test's core cases run on a
     framework thread too, since every element now gets its peer from
     the thread's `DXamlCore`.
   - *`Application.Start` (done, the end of 32.3).*
     `FrameworkApplicationFactory::StartImpl` takes the desktop path,
     `FrameworkApplication::StartDesktop`, as on Windows: a
     `DispatcherQueue` on the calling thread, the framework's core for
     islands, then the app's initialization callback, in which the app
     makes its `Application` (its own class composed over the
     framework's, as a C++/WinRT or C# app's `App` is).  The thread's
     `WindowsXamlManager.InitializeForCurrentThread` then starts the
     application on the thread (`StartOnCurrentThread`:
     `EnsureCoreApplicationInitialized` enters the application into the
     CoreWindow content root's visual tree; `ConfigureJupiterWindow`
     gives the control its desktop window object, `CJupiterWindow`,
     without a CoreWindow) and raises `OnLaunched`; the desktop message
     loop (`RunDesktopWindowMessageLoop`) runs until `Application.Exit`,
     and `Application.Start` returns.  Built from their sources for it:
     `WindowsXamlManager`, the launch and initialization arguments, the
     debug settings and error info, the core's application, visual tree,
     focus manager, time manager, layout manager and input services, the
     content root component, the focus, scaling and access key
     components, popups, `CJupiterWindow` and the control layer's
     resource manager.  NovaOS stand-ins: the lifted windowing calls the
     message loop and input make (`src/windowing.cpp`: no content
     island claims a message yet; a `WindowId` is the HWND's value), the
     closed FrameworkUdk theming and CoreWindow headers, and XAML's
     diagnostics interop, which has none to give
     (`src/diagnostics.cpp`).  ole32 now serves classes a program
     registers itself (`RoRegisterActivationFactories`, as a packaged
     app's own classes are), which the test uses for the
     `WindowsXamlManager` the framework activates, and
     `RoOriginateLanguageException` originates its error.
     `xamlapptest` starts the app, checks `Application.Current`, loads a
     `Grid` with a `TextBlock` in `OnLaunched`, and exits from work
     queued on the `DispatcherQueue`.
   - *Left for 32.4:* a `Window`.  `DesktopWindowImpl` and the
     `DesktopWindowXamlSource` under it compile, but run on the Windows
     App SDK's lifted content, input and windowing classes
     (`ContentIsland`, `DesktopChildSiteBridge`, `AppWindow`, the input
     sources and focus controller), which NovaOS does not have yet.
4. **32.4 A XAML page draws.** Layout and rendering through composition:
   a `Grid` with a `TextBlock` and a `Button` from `generic.xaml`, typed
   into and clicked in a self-test, with a reference screenshot.
   - *Content islands part (done):* the Windows App SDK's lifted
     content, input and windowing classes, which the framework reaches
     through `Microsoft.UI.Input.dll` (`MuxGetActivationFactory` in
     `xcpcore.cpp`, the registry after it), as NovaOS's own DLL
     (`userland/microsoft_ui_input`, C++ on the generated ABI headers;
     its `build.py` writes a default for every interface method, so a
     class overrides only what it does, and the rest report "not
     implemented" on the debugger's output).  `ContentIsland` holds its
     root visual, size and state; `DesktopChildSiteBridge` is a child
     window of the app's window whose composition target is the
     compositor's desktop target for it, sizes the island with itself
     (`StateChanged`) and hands its messages to the island's input
     sources (`InputPointerSource`, `InputKeyboardSource` with the
     pre-translate source, `InputFocusController`,
     `InputActivationListener`); `InputNonClientPointerSource` keeps a
     window's caption regions; `AppWindow` is the top-level window
     (subclassed for its `Changed` events) with an overlapped presenter
     and title bar.  Built with it: `DesktopWindowImpl`,
     `DesktopWindowXamlSource`, `XamlIslandRoot` (framework and core),
     the window chrome (`components/WindowChrome`) and the window's event
     arguments; the platform layer's DirectWrite font and script services
     (`pal/win`) with the text core's font families (`core/text/Fonts`).
     MinGW-w64's DirectWrite headers hide a base interface's overloads
     behind a derived interface's (C++ name hiding) where the SDK's bring
     them back with using-declarations; `build.py` generates copies that
     add them (`dwrite_headers`).  `contentislandtest` checks the layer
     on its own.
   - *Theme resources part (done):* creating a `Window` applies its
     chrome's style, which loads the framework's theme resources
     (`StyleCache::LoadThemeResources`): `ThemeResources.xbf`, and for a
     control's default style `Styles.xbf`, `TEXTFILE` resources of
     `Microsoft.UI.Xaml.Resources.19H1.dll`.  Microsoft splits
     `themes/generic.xaml` into the two (`SplitGenericXaml.cs`: the
     implicit styles; the theme dictionaries, other resources and keyed
     styles) and compiles them to XBF with the XAML compiler, which runs
     on Windows only.  NovaOS's DLL (`userland/microsoft_ui_xaml_resources_19h1`,
     its `build.py` the split in Python, compiled by `llvm-rc`) holds the
     split XAML as text, and NovaOS's `DXamlCore::GetResourceBytes`
     (`src/resource_bytes.cpp`, with the resource module handle
     Microsoft's `dllentry.cpp` keeps; DXamlCore.cpp's own renamed out,
     `FILE_RENAMES`) hands it to the parser as text, or a resource that is
     XBF as XBF.  Text differs from XBF in two ways that matter: the parser
     asks the core's resource manager about the resource's URI, so the
     platform's resource provider is built (Microsoft's
     `CommonResourceProvider`, the fallback its desktop platform takes
     when MRT Core's `ResourceManager` cannot be used, made by
     `src/resource_provider.cpp`; MRT joins with 32.6), and every type
     resolves as the text is parsed, where XBF waits until a resource is
     looked up.  The framework's metadata provider
     (`components/ExtMetadataProvider`) is built for the types the type
     table does not know, and the split leaves out the resources and
     styles of types from libraries not built yet (the phone extension
     library's `Pivot`, looping selector and picker presenters, WinUI's
     `ProgressBar` and `TextCommandBarFlyout`, and the text boxes' styles
     that name its flyouts: `OUTSIDE` in the DLL's `build.py`).  The
     framework peers the resources make as they load are built
     (`DataTemplate`, `ItemsPanelTemplate`, `TemplateContent`, the
     transitions).  `xamlapptest` finds both resources and makes the
     resource manager.
   - *Third part, DirectWrite's Windows 10 font model.*  The theme
     resources' first `FontFamily` starts XAML's text services, and text
     formatting starts from `IDWriteFactory6`.  NovaOS's DirectWrite now
     goes up to `IDWriteFactory7`: system collections in the typographic
     or weight-stretch-style family model, font sets, resources and
     references with axis values, families and lists 2, font faces 4 to
     6, font fallback 1, text formats 1 to 3, text layouts 1 to 4 and
     text analyzers 1 and 2 (`dw6test`).  NovaOS's fonts are static, so
     axis values map to weight, stretch and style.  A `Window` now loads
     the system collection and its theme resources.
   - *Fourth part, the island.*  A desktop `Window`'s
     `DesktopWindowXamlSource` makes a `XamlIsland` for its content, and
     Microsoft's open sources ship only its declarations
     (`XamlIsland_Partial.h`, `XamlIsland.g.h`).  NovaOS writes its own,
     `userland/windows_ui_xaml/src/xaml_island.cpp` (the generated class,
     its factory and the partial class): it keeps the thread's
     `WindowsXamlManager`, starts the islands on the dummy window, makes
     a `XamlIslandRoot` of the desktop content bridge kind (it connects
     when the site bridge takes its content island), hands it to the core
     (`AddXamlIslandRoot`), forwards `Content` and `ContentIsland` to it,
     keeps the system backdrop and `ShouldConstrainPopupsToWorkArea`, and
     on `Close` takes the root out of the core and disposes of it.
   - *Fifth part, the root ScrollViewer.*  Setting a page as the
     window's content puts it in a root `ScrollViewer`
     (`ContentManager::CreateRootScrollViewer`: a `RootScrollViewer` with
     scrolling and zoom off, its `ScrollContentPresenter` set by hand
     rather than by a template, and a `Border` painted with a transparent
     `SolidColorBrush` around the page).  These are Microsoft's
     (`ScrollViewer_Partial.cpp`, `ScrollContentPresenter_Partial.cpp`,
     `RootScrollViewer_Partial.cpp`, their base `ScrollContentControl`,
     `Brush` and `SolidColorBrush`, each with its generated class), with
     the core constructors of the classes the core's generator writes
     whole in headers (`core/controls/generated/GeneratedClasses.g.cpp`:
     `CRootScrollViewer`, `CScrollContentPresenter` and the rest; the
     core's `CScrollViewer` was already built).  DirectManipulation and the
     scroll bars stay unbuilt: the root's scrolling is off.
   - *Sixth part, the composition device and the first frame.*  The
     first frame asks the composition tree host for its device
     (`DCompTreeHost::EnsureDCompDevice`), made by a `CompositionHelper`
     the Windows App SDK ships closed (`inc/CompHelper/CompositionHelper.h`
     declares it).  Its main device is both a DirectComposition
     `IDCompositionDevice2` (`Commit`, `GetFrameStatistics`, surface
     factories) and the Microsoft.UI.Composition `Compositor` XAML's
     visuals are made on, so `dcomp.dll` joins its two halves: a
     `Compositor` answers `IDCompositionDevice`, `IDCompositionDevice2`
     and `3` as well (a DirectComposition device that is part of it, with
     its references and identity), and a `Commit` of that device has the
     compositor draw again.  NovaOS's `CompositionHelper`
     (`src/composition_helper.cpp`) makes a `Compositor` and keeps it as
     that device; the surfaces XAML draws into come from the device's
     surface factory on XAML's Direct3D 11 / Direct2D device and are shown
     in visuals through `dcomp.dll`'s `NovaCompositionSurfaceForDComp`
     (a DirectComposition surface as an `ICompositionSurface`).  After it
     the first frame builds the island's `FocusController`
     (`components/UIBridgeFocus`), the window's `WindowsPresentTarget`,
     the peers of the popup, print, full-window media and transition roots
     and of `Canvas`, the focus event args with their `EventArgs` base,
     `XamlSourceFocusNavigationRequest`, the core transforms' base
     `CTransform`, and Microsoft's `DependencyObjectAbstractionHelpers`
     in place of the test stubs that answered "no peer".
   - *Seventh part, the page drawn.*  The control styles load now that
     the theme transitions are built (`ThemeAnimations.cpp`, the theme
     animations helper, the visual state classes and their generated
     peers), and layout, measure and arrange run over the page
     (`ScrollData`, `RectangleGeometry` for the scroll presenter's clip,
     the transforms and `TransformGroup`, the core `SolidColorBrush`,
     `TextOptions` and `Typography`).  Two duties of the closed Windows
     App SDK code fall to NovaOS: its `XamlIsland` calls
     `CXamlIslandRoot::SetContentRequested` once the island is made
     (without it the composition tree host never connects the island's
     root visual), and its `CompositionSurfaceHelper`
     (`src/composition_helper.cpp`) begins a surface's drawing with no
     gutters (NovaOS samples surfaces nearest, so none are needed).  The
     frame is then the framework's own: the render walk and hardware walk
     (`core/hw`: the content renderers, `hwcompnode.cpp`,
     `hwrealization.cpp`), the composition nodes
     (`components/comptree`: `HWCompNode.cpp`, `HWCompNodeWinRT.cpp`)
     with their transform expressions
     (`WinRTExpressionConversionContext`, `WinRTLocalExpressionBuilder`,
     `XamlLocalTransformBuilder`), the text block's DirectWrite renderer
     (`DWriteTextRenderer`, `TextObject`) and the native text's Direct2D
     drawing context (`D2DTextDrawingContext`, `SharedWicBitmap`,
     `TextHelpers`), the text highlight renderer
     (`components/text`), the platform layer's DirectComposition
     surfaces (`DCompSurface`, `DCompSurfaceFactoryManager`,
     `DCompSurfaceMonitor`) and the shadow effect policy
     (`EffectPolicyHelper`).  NovaOS's side: `d2d1.dll` answers
     `ID2D1DeviceContext1` to `7` (XAML asks for `4` for every glyph
     run); `windowsnumerics.h` converts its `float2` ... `quaternion` to
     and from the ABI `Vector2` ... `Quaternion` as the Windows SDK's
     does; and `dcomp.dll`'s compositor gained `CompositionMaskBrush`
     (XAML draws text as its glyphs' alpha mask over a colour brush),
     `ICompositionSurfaceBrush2`/`3` (a brush's own transform, which
     places each glyph run's texture) and `ICompositionClip2` (a clip's
     transform).  `xamlpagetest` now draws the page: a SteelBlue
     `Border`, "Hello from NovaOS" in the theme's font and a "Click me"
     `Button` ([screenshot](../screenshots/winui3-first-page.png)), then
     closes the window and exits cleanly.
   - *Left for 32.4:* typing into a `TextBox` and clicking the `Button`
     in the test (the island's pointer and keyboard input into the
     framework's input manager), and a pixel check of the drawn frame.
5. **32.5 WinUI 2.** The `controls` tree as `Microsoft.UI.Xaml.dll` 2.x on
   top of the system XAML, with its `.pri` resources.
   - *First part, the framework under its Windows names.*  WinUI 2 (the
     `winui2/main` branch of microsoft-ui-xaml, C++/WinRT) is compiled
     against `Windows.UI.Xaml`: Windows' interface IDs and Windows'
     interface split (`IUIElement`, `IUIElement2` ... `IUIElement10`),
     where the framework's source is WinUI 3's, with the lifted
     `Microsoft.UI.Xaml` interfaces (one `IUIElement` holding them all,
     some members moved: `Control.IsTabStop` is `UIElement`'s).  WinUI
     2's own classes are named `Microsoft.UI.Xaml.Controls.*` too, so the
     framework cannot answer under its lifted names alone.  The
     namespace is therefore given as faces rather than by building the
     framework twice: `tools/winrt/winfaces.py` reads Windows' metadata
     with the framework's IDL (as `abigen.py` does) and writes
     `windows_faces.g.cpp`, a table of faces, each a vtable of thunks:
     *forward* faces implement a `Windows.UI.Xaml` interface by calling
     the framework interface whose method has the same name (the class's
     own, a base class's, or for statics a base class's factory), and
     *reverse* faces implement a framework interface over a Windows one:
     the overridable interfaces (what the framework calls on a class an
     app composed over its own), delegates (event handlers), the
     interfaces an app implements (`ICommand`, `IValueConverter`) and the
     collections' generic instances (`IVector<UIElement>`).  A thunk
     converts what it passes: an interface pointer is asked for the other
     world's interface (or wrapped in the face that gives it), a Windows
     box of a `Windows.UI.Xaml` enum or struct (`IReference<Visibility>`,
     as C++/WinRT boxes it) is boxed again as the framework's, and a
     `TypeName` naming a `Windows.UI.Xaml` type is renamed as the
     framework's and back.  A method whose types have no counterpart (a
     `Windows.UI.Composition` visual where the framework takes
     `Microsoft.UI.Composition`'s, an array of objects) answers
     `E_NOTIMPL` and names itself once on the debugger's output.
     The framework objects' `QueryInterface` (NovaOS's
     `src/ComObjectBase.cpp` in place of components/com's) hands out a
     forward face's tear-off for a `Windows.UI.Xaml` interface ID the
     object has none of its own for, and, asked by a composed object's
     outer for a framework overridable interface the outer passed on,
     the reverse face over the outer's Windows overrides (which call the
     framework's own back when the app does not override them).  The
     framework's own table of activation factories (`Factories.g.cpp`,
     behind Microsoft's `DllGetActivationFactory`) gives its classes by
     their Windows names (`src/windows_activation.cpp`).  A face handed
     back as an object is the object it is a face of, so the framework
     only ever holds its own objects.  ole32 gained `RoParseTypeName`
     (wintypes' `api-ms-win-ro-typeresolution`), with which the
     framework reads a box's element type from its class name.  Test:
     `wuxamltest`, a `Windows.UI.Xaml` client: a `Button` activated by
     its Windows name and driven through Windows' interfaces (one
     identity, its Windows class name), a Windows-boxed `Visibility`,
     `DependencyProperty.Register` with Windows type names,
     `XamlReader.Load` and the page's children through Windows'
     `IVector<UIElement>`, `Control.IsTabStopProperty`, `Click` with a
     Windows handler, and a control composed over
     `Windows.UI.Xaml.Controls.Control` whose `MeasureOverride` the
     framework's layout calls.  (A `Visibility` set on a `Button` goes to
     its visual states, `VisualStateManager`, which 32.4 builds; the test
     sets it on a `TextBlock`.)
   - *Second part, WinUI 2's controls built.*  `userland/microsoft_ui_xaml`
     builds `Microsoft.UI.Xaml.dll` 2.x from WinUI 2's own sources
     (`third_party/winui2`, the `winui2/main` branch, MIT) the way its
     Visual Studio project does, with no Windows SDK tool:
     - *Metadata.*  The features' IDL (Microsoft.UI.Xaml.idl imports each
       one's, MIDL 3 with some MIDL 2: the composition effects) is read by
       `tools/winrt/idl.py` (taught `feature_name`, `midl_pragma`,
       forward declarations, MIDL 2 typedefs and `[propget]` methods) into
       the model abigen uses, and `tools/winrt/winmdwrite.py` writes it as
       a `.winmd` (ECMA-335 tables in a PE file, the attributes C++/WinRT
       reads: `Guid`, `Activatable`, `Composable`, `Static`, `ExclusiveTo`,
       `Default`, `Overridable`, `Overload` ...), where MIDL would.
     - *Projection.*  C++/WinRT's code generator (`third_party/cppwinrt`,
       2.0.250303.1, MIT) is built for the build machine and writes the
       projection of Windows' metadata (`third_party/winmd`) and WinUI 2's
       component files (`module.g.cpp`, each class's `.g.h`), as the
       NuGet package's MSBuild targets run it.
     - *Type table.*  `xamlmeta.py` writes `XamlMetadataProviderGenerated.cpp`
       as Microsoft's T4 template (`dll/XamlMetadataProviderGenerated.tt`)
       does: every public type, its base type, activator and properties.
     - *Compiling.*  clang (C++20, no RTTI) over the precompiled header the
       project uses; MSVC-only spellings in three vendored files are
       written for clang (listed in `third_party/winui2/NOVA-VENDOR.txt`);
       the SDK headers WinUI 2 includes that MinGW-w64 lacks are in
       `userland/microsoft_ui_xaml/inc`.
     - *Features so far:* Microsoft's inner-loop set (the shared code,
       collections, composition effects, materials, lights, icons,
       `AnimatedVisualPlayer`, `AnimatedIcon`, `SplitButton`,
       `DropDownButton`) and `InfoBadge`.  The rest are added by
       copying their folders and listing them in `FEATURES`.
     - *Running it.*  A WinUI 2 app's `Application` is its
       `IXamlMetadataProvider`, forwarding to WinUI 2's
       (`XamlControlsXamlMetaDataProvider`); the framework's metadata
       (components/ExtMetadataProvider, now built) asks it for the types
       it does not know, and the faces give the framework the app's
       Windows interface as its own (`nova_xaml_outer_face`); a framework
       method Windows moved to a later interface (`IXamlType`'s
       `BoxedType` is `IXamlType2`'s) is asked of that one, and answers
       nothing when the object lacks it, as WinUI 2's types do.  For a
       control's property-changed callbacks the framework's
       `DependencyPropertyChangedEventArgs` and the visual state machine
       under `VisualStateManager` (components/vsm) are built.  WinUI 2's
       controls compose over the framework's `Windows.UI.Xaml` classes,
       make `Windows.Foundation.Uri`s (urlmon.dll serves the class, as on
       Windows) and register their dependency properties on the
       framework.  Test `winui2test`: an `InfoBadge` and a
       `DropDownButton` made through WinUI 2's type table, the framework's
       interfaces behind them, and `InfoBadge.Value` read, set and read
       back.
   - *Third part, what an app compiled against the NuGet package needs.*
     - *Interface IDs.*  MIDL 3 makes up the IDs of the interfaces it
       synthesizes for a class (its default, factory, statics,
       overridable and protected interfaces) by an algorithm Microsoft has
       not published, and WinUI 2's IDL names none for them, so an app
       compiled against the `Microsoft.UI.Xaml` package asks for IDs the
       IDL cannot give.  `userland/microsoft_ui_xaml/iids.txt` lists the
       package's own (2.8.7, Calculator's: 402 interfaces;
       `muxiids.py` reads them from its `Microsoft.UI.Xaml.winmd` and
       writes the list, which is kept as facts, as MinGW-w64's headers keep
       Windows' IDs; the package itself is not part of the build), and the
       build gives each interface its published ID, abigen's name-based one
       only to an interface the package lacks (2.9's additions, the
       private test hooks).  An ID the IDL does name must equal the
       published one, or the build stops; the interfaces both have list
       their methods in the same order.
     - *XAML resources.*  Each feature's XAML pages (a control's default
       style, its theme resources, in a Version2 and a Version1 look) are
       merged as Microsoft's build merges them: its MSBuild tasks
       (`BatchMergeXaml`, `MergedDictionary`, `StripNamespaces`, from
       microsoft-ui-xaml's `tools/CustomTasks`) ported to
       `mergexaml.py`, the pages ordered and sorted into releases as
       `Microsoft.UI.Xaml.Common.targets` does (each release's merge over
       the previous one's, one entry per key, the theme dictionaries
       merged by theme, conditional XAML resolved for the release's
       `UniversalApiContract`).  `CommonStyles` (WinUI 2's styles of the
       framework's own controls) is merged with the features built.
       `XamlControlsResources` and each control's
       `DefaultStyleResourceUri` choose the release by the contracts
       `ApiInformation` reports; NovaOS reports Windows 10 22H2
       (contract 10), so the 19H1 files are installed,
       `C:\Programs\Microsoft.UI.Xaml\Themes\19h1_themeresources.xaml`,
       `19h1_compact_themeresources.xaml` and `19h1_generic.xaml` (each also
       in its `_v1` look), where `ms-appx:///Microsoft.UI.Xaml/Themes/`
       finds them for an unpackaged program in `C:\Programs` (as the NuGet
       package copies them into an app's folder); WinUI 2 asks its framework
       package, `ms-appx://Microsoft.UI.Xaml.2.x/`, only when it runs from
       one (32.6).  Microsoft compiles them to
       XBF; the XAML compiler is Windows-only, so they stay XAML text,
       which the framework's parser reads.  XAML text resolves every type
       as it parses, so the resources that name a WinUI 2 type not built
       yet (`TextCommandBarFlyout`, `NavigationViewItemPresenter`), or
       use markup the framework cannot make yet (data binding,
       `{Binding}`, before the fourth part), or name such a resource, are
       left out; the build lists them in `Themes/left-out.txt`
       (`FRAMEWORK_LACKS` in `userland/microsoft_ui_xaml/build.py` is now
       empty).  Test: `winui2test` checks the files are installed.
   - *Fourth part, data binding, control templates and the UWP start-up.*
     - *Data binding.*  The framework's own `Binding`, `BindingExpression`,
       `RelativeSource` and `PropertyPath` (dxaml/lib's `Binding_Partial`,
       `BindingExpression_Partial`, the property path parser and steps,
       the property accessors for dependency properties, `ICustomPropertyProvider`,
       indexers and maps, the value converters), the markup extensions'
       service provider the parser hands a `{Binding}`
       (`ParserServiceProvider`, `ProvideValueTargetProperty`,
       `MarkupExtension`), the effective value a property set by an
       expression keeps (`EffectiveValueEntry`), `VisualTreeHelper`, and
       the framework's `EventArgs` (`FrameworkEventArgs.cpp`, which
       `DirectUI::EventArgs` lived in, unbuilt until now).
     - *Control templates.*  The style component (components/style:
       `CStyle` and the optimized style its setters are read from, which
       hands a control its `Template`), `{TemplateBinding}` to a property
       the core does not bind itself (`TemplateBindingExpression`), and
       the core's create functions of the framework-only types
       (core/controls/generated: the theme transitions the default styles
       make, with their states, `LayoutTransition_partial`,
       `ThemeGenerator` and the stagger functions; uxtheme's animation
       queries answer that no theme is open, as Windows' do without one,
       and the framework uses its built-in timings).  The parser's errors
       now carry their text (`src/error_strings.cpp`, from
       win/agcore.debug/CommonErrors.rc, as a table written at build time).
       Test: `winui2test` loads `{Binding}`s to the element itself, its
       `DataContext` and a template's templated parent, makes a `Binding`
       in code and sees it follow its source, and applies templates
       (plain, `{TemplateBinding}`, `{Binding}` to the templated parent, a
       `Style`'s template), with `VisualTreeHelper` finding the template's
       root.
     - *UWP start-up.*  `Application.Start` takes its UWP route when
       `AppPolicyGetWindowingModel` says Universal, which kernel32 says
       for a packaged app whose entry point is not
       `Windows.FullTrustApplication`.  The framework's view source and
       view (`FrameworkViewSource_Partial`, `FrameworkView_Partial`) and
       its `CoreWindow` wrapper are built; the core's `EnableUWPWindow`
       feature is on (`HKLM\SOFTWARE\Microsoft\WinUI\Xaml`);
       twinapi.appcore's `CoreApplication` answers `ICoreApplication2`
       (the background events and prelaunch) and serves `CoreCursor`.
       `winui2test` packs itself as a UWP app (`tools/appx/mkuwptest.py`,
       `C:\Tests\Appx\XamlUwpTest.msix`), installs it, starts it by its
       AUMID through `ApplicationActivationManager` and reads its log:
       the package identity, the Universal windowing model and the app's
       `Application` made pass.
     - *Where it stops.*  `Application.Start`'s UWP route stops at the
       core's `DispatcherTimer` (the core services' background resource
       timer, `CCoreServices::EnsureBackgroundTimer`): it is a `CTimeline`,
       whose constructor and composition half are the animation component
       (components/animation, with `Timeline.cpp`'s unpublished
       `CompHelper/CompositionAnimationHelper.h`), not built yet.  WinUI
       2's theme resources (`XamlControlsResources`) stop at
       `AcrylicBrush`'s `FallbackColor` (`AG_E_PARSER2_UNKNOWN_PROP_ON_TYPE`):
       the property is its base's, the framework's
       `XamlCompositionBrushBase` (built now, with its core half); WinUI
       2's type table names that base and answers no member of it, as on
       Windows, and the framework's walk from the WinUI 2 type to its
       Windows base does not find the property yet;
       without those resources an `InfoBadge`'s `ApplyTemplate` finds no
       default style.  `winui2test` reports these as "not yet" lines.
   - *Fifth part, the animation and WinUI 2's inherited properties.*
     - *Animation.*  The core's animation library whole (core/animation:
       timelines, storyboards, the animations and key frames, key times,
       durations, repeat behaviours, triggers and the timer under
       `DispatcherTimer`) and the animation component
       (components/animation: the timelines' and animations' second
       halves, the conversion of an animation to a composition one, the
       easing functions, connected animations); of the framework,
       `DispatcherTimer` with its timeline timer, `Timeline`, `Storyboard`
       and the generated classes of the animations, key frames and
       eases.  The composition animations start through
       `CompositionAnimationHelper`, whose Windows App SDK header and code
       are closed: NovaOS's (`inc/CompHelper/CompositionAnimationHelper.h`,
       `src/composition_animation_helper.cpp`) starts and stops an
       animation on the object's property through NovaOS's
       `Windows.UI.Composition`, which keeps no animation controllers, so
       its pause and seek leave the animation running.  Also: the time
       manager's composition batch handler lists `FtmBase` without
       `IAgileObject` (NovaOS's WRL `FtmBase` derives it), an SDK-style
       `<minmax.h>`, and the
       effects headers moved from WinUI 2's include directory to the
       framework's, which both build with.
     - *FrameworkUdk's `CoreWindow` position listener*
       (`WindowPositionChanged_RegisterListener`, `src/framework_udk.cpp`):
       a UWP app's window registers to hear its `CoreWindow` move; the
       listeners are kept and not called yet.
     - *Inherited properties of WinUI 2 types.*  WinUI 2's type table names
       the Windows types its types derive from as primitive type names
       (`Windows.UI.Xaml.Media.XamlCompositionBrushBase` under
       `AcrylicBrush`); the faces now give the framework those names under
       its own (`nova_typename`), so the framework's walk from a WinUI 2
       type to its base finds the base's properties, `FallbackColor`
       among them.  Test: `winui2test` loads an `AcrylicBrush` with its
       `FallbackColor` from markup.
     - *Where it stops.*  `Application.Start`'s UWP route now passes the
       core's `DispatcherTimer` and reaches the view's `SetWindow`, which
       sizes the window and stops at its windowed present target
       (`WindowsPresentTarget::CreateWindowedPresentTarget`, step 32.4's
       sixth part).  `XamlControlsResources` now loads past
       `AcrylicBrush` and stops at `InkToolbarFlyoutItem`, a Windows
       XAML type the framework does not have.
   - *Sixth part, the UWP start-up through and the theme resources further.*
     - *UWP start-up.*  Inside its package `winui2test` now runs
       `Application.Start`'s UWP route to the end: `Window.Current` is the
       view's window, with its `CoreWindow`; content set, `Activate` and
       `Application.Exit` (15 checks).  On the way: `GetDebugInterop`
       answers null when no debug tool is attached (NovaOS's WRL
       `WeakRef::CopyTo` of an empty reference fails, a vendored edit
       in `DXamlCore.cpp` rather than a change to the widely included
       `wrl/client.h`), twinapi.appcore's `CoreWindow` has `ICoreWindow5`
       (its `DispatcherQueue` and `ActivationMode`, which the focus
       observer asks), and the generated `WindowCreatedEventArgs` class.
     - *Windows-only types left out.*  `leave_out_unbuilt`
       (`userland/microsoft_ui_xaml/build.py`) now also checks the names
       the presentation namespace gives (no prefix) against the
       framework's own type table (`StaticMetadata.g.cpp`) and WinUI 2's
       types: resources naming a type neither has are Windows-only
       controls the framework lacks (`InkToolbar*` from
       Windows.UI.Xaml.InkControls.dll, `Pivot`, `LoopingSelector`,
       `MediaTransportControls`) and are left out, with what names them.
     - *Attached and content properties.*  WinUI 2's type table
       (`xamlmeta.py`) registers an attached property (a static `Get`
       method, no instance property) as attachable and a class's
       `[contentproperty]` as its content: parsing XAML text needs both,
       where Microsoft's XBF compiler resolves them from the metadata
       (`local:AnimatedIcon.State`, an `AnimatedIcon`'s source as content).
     - *What WinUI 2's brushes ask the system.*  `DesignMode` and
       `PowerManager` (from `GetSystemPowerStatus`; its events are not
       raised yet) in twinapi.appcore; `Window.Current` on a desktop thread
       gives the core's window, as Windows.UI.Xaml does for XAML islands
       (WinUI 3 gives null); the framework's `CompositionTarget` (its
       `Rendering` event) and `XamlLight`.
     - *Where it stops.*  `XamlControlsResources` now loads past the
       acrylic brushes and stops at `RevealBrush.TargetTheme`: WinUI 2's
       reveal code fails `E_NOINTERFACE` when the property changes.
   - *Seventh part, the theme resources whole.*  `XamlControlsResources`
     loads from its `Source` and answers lookups (`winui2test`: the
     dictionary, `ControlNormalAnimationDuration`, the dark theme's
     `AcrylicBackgroundFillColorDefaultBrush` tinted).
     - *Composition types across the faces.*  The `RevealBrush` stop was
       a missing face: `XamlCompositionBrushBase`'s protected
       `CompositionBrush` takes a `Windows.UI.Composition.CompositionBrush`
       in Windows' metadata and a `Microsoft.UI.Composition` one in the
       framework's, which `tools/winrt/winfaces.py` never matched, so the
       interface had no face and WinUI 2's `.as<>` threw.  The
       compositor's objects (dcomp.dll) answer both namespaces' IDs, so
       `same_composition()` now lets a face pass such an object across
       (ten more faces, among them `UIElement.StartAnimation`).
     - *Resources in lookup order.*  XBF creates a resource when it is
       first looked up, so WinUI 2's dictionaries name resources defined
       later (`BasedOn="{StaticResource DefaultMenuFlyoutItemStyle}"`
       before that style).  XAML text does not: the framework's
       `CDeferredKeys` lets a resource being created (and an implicit
       style, created as the dictionary loads) see only the keys before
       it.  `lookup_order()` (`userland/microsoft_ui_xaml/build.py`)
       reorders each merged dictionary so a resource follows those it
       names, the merged order kept otherwise.
     - *Acrylic brushes.*  Windows has `Windows.UI.Xaml.Media.AcrylicBrush`
       (built from the same source as WinUI 2's); WinUI 3's framework has
       none, so the theme resources' presentation-namespace `AcrylicBrush`
       is WinUI 2's.  `XamlControlsResources` then asks it for Windows'
       `IAcrylicBrush`; the framework's query answers Windows'
       `IAcrylicBrush` and `IAcrylicBrush2` with the outer's WinUI 2
       interfaces, which have the same methods in the same order
       (`windows_faces.cpp`, `s_winui2_aliases`).
   - *Next:* the controls' default styles applied (`InfoBadge`'s template:
     the style is in the theme resources, which now load);
     then the features Calculator uses
     (`NavigationView` with `ItemsRepeater`, `SwipeControl`,
     `RadioButtons`, `ProgressRing`, `BackdropMaterial`).  Calculator
     itself is a .NET Native app: its package also needs the .NET Native
     framework and runtime packages (32.6).
6. **32.6 Packages.** Install an `.msix`/`.appx` (manifest, activatable
   classes from `InProcessServer` extensions, `resources.pri` read by
   MRT), start it from the Start menu; then the Calculator package
   (roadmap 27.x leftovers).  Done so far: `appx.exe` (userland/programs/
   appx.c) unpacks an unsigned package into `C:\Programs\WindowsApps\
   <full name>` after checking it against its block map, and records it
   in the user's package repository (`HKCU\Software\Classes\Local
   Settings\...\AppModel\Repository`: `Packages\<full>` with
   `PackageRootFolder` and `Applications\<id>`, `Families\<family>`),
   its classes under `HKCU\Software\Classes\ActivatableClasses\
   Package\<full>`.  kernel32's `package.c` gives a process the package
   whose folder holds its program; ole32 looks up its classes there
   first; `ApplicationActivationManager` (twinapi.appcore) starts an
   application by AUMID.  `MRM.dll` is MRT Core built unchanged, and
   `SHLoadIndirectString` reads `ms-resource:` strings with it.  A signed
   package is refused until NovaOS verifies package signatures (PKCS#7
   over the block map and the chain, as Windows does); bundles and
   framework packages come with Calculator.

Out-of-process WinRT servers, the app container sandbox and the
.NET Native/UWP .NET runtime are later work.
