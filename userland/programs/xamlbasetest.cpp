/* xamlbasetest.exe — the first of Microsoft's XAML framework built for
 * NovaOS (roadmap step 32.3, docs/design/windows-ui-xaml.md): the XAML
 * core's lowest layer (dxaml/xcp/components: strings, numbers, containers,
 * resource qualifiers, runtime-enabled features, the dependency locator)
 * its type metadata (every built-in type and dependency property, by
 * index and by name) and the collections on CDependencyObject, compiled
 * from Microsoft's sources as XAML is compiled and linked from
 * xaml_components.lib, plus NovaOS's WRL (the Windows SDK's Windows
 * Runtime C++ template library the sources are written on): ComPtr,
 * RuntimeClass, weak references, Callback, EventSource, HString.  What
 * those call in the XAML core above them (elements, the core services,
 * custom types' providers) is Microsoft's own stubs and metadata mocks from
 * its isolated unit tests (components/unittests), from xaml_teststubs.lib,
 * until the core builds; the test is built as those tests are, with
 * __XAML_UNITTESTS__ (xamlbasetest.json).
 *
 *   xamlbasetest        the self-test runs it with no arguments
 */
#include <stdio.h>
#include <wrl.h>
#include <windows.foundation.h>
#include <xstring_ptr.h>
#include <XStringBuilder.h>
#include <DoubleUtil.h>
#include <MinMath.h>
#include <bit_vector.h>
#include <QualifierContext.h>
#include <QualifierFactory.h>
#include <RuntimeEnabledFeatures.h>
#include <DependencyLocator.h>
#include <Indexes.g.h>
#include <TypeTableStructs.h>
#include <MetadataAPI.h>
#include <CStaticLock.h>
#include <DOCollection.h>
#include <CollectionMoveView.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
namespace wf = ABI::Windows::Foundation;

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); fflush(stdout); } } while (0)

/* a Windows Runtime object on RuntimeClass: IClosable, weak references */
static int closed, destroyed;
class Closable : public RuntimeClass<wf::IClosable>
{
    InspectableClass(L"NovaOS.Test.Closable", BaseTrust)
public:
    ~Closable() { destroyed++; }
    IFACEMETHODIMP Close() override { closed++; return S_OK; }
};

/* a classic COM object on RuntimeClass, with a cloaked interface */
struct __declspec(uuid("6c3e2b8a-91f4-4d6e-a8f2-3b5d7c9e1a20")) ICounter : public IUnknown
{
    virtual int STDMETHODCALLTYPE Next() = 0;
};
struct __declspec(uuid("6c3e2b8a-91f4-4d6e-a8f2-3b5d7c9e1a21")) IHidden : public IUnknown
{
    virtual int STDMETHODCALLTYPE Secret() = 0;
};
class Counter : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICounter, CloakedIid<IHidden>>
{
    int n = 0;
public:
    int STDMETHODCALLTYPE Next() override { return ++n; }
    int STDMETHODCALLTYPE Secret() override { return 42; }
};

static void test_wrl()
{
    {
        ComPtr<Closable> c = Make<Closable>();
        CHECK("Make", c && c->GetRefCount() == 1);
        ComPtr<wf::IClosable> i;
        CHECK("As IClosable", SUCCEEDED(c.As(&i)) && i);
        ComPtr<IInspectable> insp;
        CHECK("As IInspectable", SUCCEEDED(i.As(&insp)));
        HSTRING name = nullptr;
        CHECK("GetRuntimeClassName", SUCCEEDED(insp->GetRuntimeClassName(&name)) &&
              !wcscmp(WindowsGetStringRawBuffer(name, nullptr), L"NovaOS.Test.Closable"));
        WindowsDeleteString(name);
        ComPtr<IAgileObject> agile;
        CHECK("agile (free-threaded)", SUCCEEDED(i.As(&agile)));
        ComPtr<wf::IStringable> none;
        CHECK("QI not implemented", i.As(&none) == E_NOINTERFACE && !none);
        i->Close();
        CHECK("Close called", closed == 1);

        WeakRef weak;
        CHECK("AsWeak", SUCCEEDED(i.AsWeak(&weak)));
        ComPtr<wf::IClosable> back;
        CHECK("weak resolves while alive", SUCCEEDED(weak.As(&back)) && back.Get() == i.Get());
        back.Reset();
        i.Reset();
        insp.Reset();
        agile.Reset();
        c.Reset();
        CHECK("destroyed at last release", destroyed == 1);
        CHECK("weak resolves to null after", SUCCEEDED(weak.As(&back)) && !back);
    }
    {
        ComPtr<ICounter> k;
        CHECK("Make classic", SUCCEEDED(Make<Counter>().As(&k)) && k && k->Next() == 1 && k->Next() == 2);
        ComPtr<IHidden> h;
        CHECK("cloaked QI", SUCCEEDED(k.As(&h)) && h->Secret() == 42);
        ComPtr<IInspectable> insp;
        CHECK("classic is not IInspectable", FAILED(k.As(&insp)));
    }
    {
        int calls = 0;
        wf::AsyncStatus seen = wf::AsyncStatus::Started;
        auto cb = Callback<wf::IAsyncActionCompletedHandler>([&](wf::IAsyncAction *, wf::AsyncStatus s) -> HRESULT {
            calls++;
            seen = s;
            return S_OK;
        });
        CHECK("Callback", cb && SUCCEEDED(cb->Invoke(nullptr, wf::AsyncStatus::Completed)) && calls == 1 && seen == wf::AsyncStatus::Completed);

        EventSource<wf::IAsyncActionCompletedHandler> ev;
        EventRegistrationToken t1, t2;
        CHECK("EventSource Add", SUCCEEDED(ev.Add(cb.Get(), &t1)) && SUCCEEDED(ev.Add(cb.Get(), &t2)) && ev.GetSize() == 2);
        CHECK("InvokeAll", SUCCEEDED(ev.InvokeAll(nullptr, wf::AsyncStatus::Error)) && calls == 3 && seen == wf::AsyncStatus::Error);
        ev.Remove(t1);
        CHECK("Remove", ev.GetSize() == 1 && SUCCEEDED(ev.InvokeAll(nullptr, wf::AsyncStatus::Canceled)) && calls == 4);
    }
    {
        HString s;
        CHECK("HString Set", SUCCEEDED(s.Set(L"Hello")) && s.Length() == 5);
        HStringReference r(L"Hello");
        CHECK("HStringReference compares", s == r);
        HString d;
        CHECK("Duplicate", SUCCEEDED(s.CopyTo(d.GetAddressOf())) && d == s);
        CHECK("empty HStringReference is null", HStringReference(L"").Get() == nullptr);
    }
}

/* the XAML core's base components */
static void test_components()
{
    xstring_ptr a;
    CHECK("xstring_ptr CloneBuffer", SUCCEEDED(xstring_ptr::CloneBuffer(L"Grid.Row", &a)) && a.GetCount() == 8);
    CHECK("xstring_ptr Equals", a.Equals(XSTRING_PTR_EPHEMERAL(L"Grid.Row")));
    CHECK("xstring_ptr case-insensitive",
          a.Equals(XSTRING_PTR_EPHEMERAL(L"grid.row"), xstrCompareCaseInsensitive) && !a.Equals(XSTRING_PTR_EPHEMERAL(L"grid.row")));
    CHECK("xstring_ptr FindChar", a.FindChar(L'.') == 4);
    CHECK("xstring_ptr StartsWith", a.StartsWith(XSTRING_PTR_EPHEMERAL(L"Grid")));

    XStringBuilder b;
    xstring_ptr built;
    CHECK("XStringBuilder", SUCCEEDED(b.Initialize()) && SUCCEEDED(b.Append(XSTRING_PTR_EPHEMERAL(L"Text"))) &&
          SUCCEEDED(b.AppendChar(L'B')) && SUCCEEDED(b.Append(L"lock", 4)) && b.GetCount() == 9 &&
          SUCCEEDED(b.DetachString(&built)) && built.Equals(XSTRING_PTR_EPHEMERAL(L"TextBlock")));
    xstring_ptr joined;
    CHECK("Concatenate", SUCCEEDED(xstring_ptr::Concatenate(a, 0, built, 4, &joined)) &&
          joined.Equals(XSTRING_PTR_EPHEMERAL(L"Grid.RowBlock")));
    xstring_ptr num;
    CHECK("CreateFromUInt32", SUCCEEDED(xstring_ptr::CreateFromUInt32(1234, &num)) && num.Equals(XSTRING_PTR_EPHEMERAL(L"1234")));
    HStringReference h(L"from HSTRING");
    xstring_ptr fromh;
    CHECK("CloneRuntimeStringHandle", SUCCEEDED(xstring_ptr::CloneRuntimeStringHandle(h.Get(), &fromh)) &&
          fromh.Equals(XSTRING_PTR_EPHEMERAL(L"from HSTRING")));

    using DirectUI::DoubleUtil;
    CHECK("DoubleUtil NaN", DoubleUtil::IsNaN(DoubleUtil::NaN) && !DoubleUtil::IsNaN(1.0));
    CHECK("DoubleUtil infinity", DoubleUtil::IsPositiveInfinity(DoubleUtil::PositiveInfinity) &&
          DoubleUtil::IsInfinity(DoubleUtil::NegativeInfinity));
    CHECK("DoubleUtil AreClose", DoubleUtil::AreClose(0.1 + 0.2, 0.3) && !DoubleUtil::AreClose(1.0, 1.1));
    CHECK("DoubleUtil Max/Floor", DoubleUtil::Max(2.0, 3.5) == 3.5 && DoubleUtil::Floor(2.7) == 2.0);

    containers::bit_vector bits;
    for (int i = 0; i < 100; i++) bits.push_back(i % 3 == 0);
    CHECK("bit_vector grows past one word", bits.size() == 100 && bits[0] && !bits[1] && bits[99]);
    bits.erase(0);
    CHECK("bit_vector erase", bits.size() == 99 && !bits[0] && bits[2]);
    bits.reset(10);
    CHECK("bit_vector reset", bits.size() == 10 && bits.all_false());

    /* resource qualifiers (a ResourceDictionary's MinWindowWidth...) */
    QualifierContext ctx;
    auto wide = QualifierFactory::Create(QualifierFlags::Width, 720);
    CHECK("MinWidth qualifier", wide && wide->Flags() == QualifierFlags::Width);
    ctx.OnWindowChanged(1024, 768);
    wide->Evaluate(&ctx);
    CHECK("qualified at 1024", wide->IsQualified());
    ctx.OnWindowChanged(640, 480);
    wide->Evaluate(&ctx);
    CHECK("not qualified at 640", !wide->IsQualified());

    using namespace RuntimeFeatureBehavior;
    auto features = GetRuntimeEnabledFeatureDetector();
    CHECK("RuntimeEnabledFeatureDetector", features != nullptr);
    if (features) {
        CHECK("feature defaults", features->IsFeatureEnabled(RuntimeEnabledFeature::TestEnabledByDefaultFeature) &&
              !features->IsFeatureEnabled(RuntimeEnabledFeature::TestDisabledByDefaultFeature));
        features->SetFeatureOverride(RuntimeEnabledFeature::TestDisabledByDefaultFeature, true);
        CHECK("feature override", features->IsFeatureEnabled(RuntimeEnabledFeature::TestDisabledByDefaultFeature));
        features->ClearAllFeatureOverrides();
        CHECK("override cleared", !features->IsFeatureEnabled(RuntimeEnabledFeature::TestDisabledByDefaultFeature));
        CHECK("dependency locator gives one instance", GetRuntimeEnabledFeatureDetector() == features);
    }
}

/* the type metadata: XAML's built-in types and properties (StaticMetadata.g.cpp) */
static void test_metadata()
{
    using DirectUI::MetadataAPI;
    const CClassInfo *button = MetadataAPI::GetClassInfoByIndex(KnownTypeIndex::Button);
    CHECK("Button class info", button && button->GetIndex() == KnownTypeIndex::Button &&
          button->GetName().Equals(XSTRING_PTR_EPHEMERAL(L"Button")));
    CHECK("Button full name", button && button->GetFullName().Equals(XSTRING_PTR_EPHEMERAL(L"Microsoft.UI.Xaml.Controls.Button")));
    int depth = 0;
    const CClassInfo *t = button;
    while (t && t->GetIndex() != KnownTypeIndex::DependencyObject && depth < 32) { t = t->GetBaseType(); depth++; }
    CHECK("Button derives from DependencyObject", t && t->GetIndex() == KnownTypeIndex::DependencyObject && depth >= 4);

    CHECK("IsAssignableFrom", MetadataAPI::IsAssignableFrom(KnownTypeIndex::DependencyObject, KnownTypeIndex::FrameworkElement) &&
          MetadataAPI::IsAssignableFrom(KnownTypeIndex::ListViewBase, KnownTypeIndex::ListView) &&
          !MetadataAPI::IsAssignableFrom(KnownTypeIndex::Control, KnownTypeIndex::DependencyObject) &&
          !MetadataAPI::IsAssignableFrom(KnownTypeIndex::FrameworkElement, KnownTypeIndex::Application));

    const CClassInfo *byName = nullptr;
    CHECK("GetClassInfoByFullName", SUCCEEDED(MetadataAPI::GetClassInfoByFullName(
              XSTRING_PTR_EPHEMERAL(L"Microsoft.UI.Xaml.Controls.Grid"), &byName)) && byName &&
          byName->GetIndex() == KnownTypeIndex::Grid);

    const CDependencyProperty *width = MetadataAPI::GetDependencyPropertyByIndex(KnownPropertyIndex::FrameworkElement_Width);
    CHECK("FrameworkElement.Width", width && width->GetName().Equals(XSTRING_PTR_EPHEMERAL(L"Width")) &&
          width->GetDeclaringTypeIndex() == KnownTypeIndex::FrameworkElement &&
          width->GetPropertyTypeIndex() == KnownTypeIndex::Double);
    const CDependencyProperty *row = MetadataAPI::GetDependencyPropertyByIndex(KnownPropertyIndex::Grid_Row);
    CHECK("Grid.Row is attached", row && row->IsAttached() && row->GetName().Equals(XSTRING_PTR_EPHEMERAL(L"Row")));

    const CDependencyProperty *found = nullptr;
    CHECK("TryGetDependencyPropertyByName", SUCCEEDED(MetadataAPI::TryGetDependencyPropertyByName(
              button, XSTRING_PTR_EPHEMERAL(L"Content"), &found)) && found &&
          found->GetIndex() == KnownPropertyIndex::ContentControl_Content);
    found = nullptr;
    CHECK("inherited property by name (Width on Button)", SUCCEEDED(MetadataAPI::TryGetDependencyPropertyByName(
              button, XSTRING_PTR_EPHEMERAL(L"Width"), &found)) && found == width);
    found = nullptr;
    CHECK("Name directive", SUCCEEDED(MetadataAPI::TryGetDependencyPropertyByName(
              button, XSTRING_PTR_EPHEMERAL(L"Name"), &found, true)) && found &&
          found->GetIndex() == KnownPropertyIndex::DependencyObject_Name);
    found = nullptr;
    CHECK("unknown property", SUCCEEDED(MetadataAPI::TryGetDependencyPropertyByName(
              button, XSTRING_PTR_EPHEMERAL(L"NoSuchProperty"), &found)) && !found);
}

/* the collections: CDOCollection (a collection of dependency objects, what
 * Panel.Children and the rest build on, made as Microsoft's isolated tests
 * make it, with no core), and the view a collection's Move keeps of its
 * items while it moves them */
static void test_collections()
{
    /* Microsoft's own collection tests: a bare CDOCollection has no type
     * index, so ValidateItem refuses every item; the typed
     * DependencyObjectCollection takes any DependencyObject */
    auto bare = make_xref<CDOCollection>();
    CHECK("CDOCollection made", bare && bare->GetCount() == 0);
    CHECK("Reserve", bare->Reserve(33) == S_OK);
    CHECK("associates children", bare->ShouldAssociateChildren(nullptr));
    auto items = make_xref<CDependencyObjectCollection>();
    xref_ptr<CDependencyObject> child[4];
    for (auto &c : child) c.attach(new CDependencyObject());
    for (auto &c : child) items->push_back(c);
    auto &list = items->GetCollection();
    CHECK("push_back", list.size() == 4 && list[0] == child[0].get() && list[3] == child[3].get());
    XINT32 at = -1;
    CHECK("IndexOf", SUCCEEDED(items->IndexOf(child[2].get(), &at)) && at == 2);
    CHECK("child knows its parent", child[1]->GetParentInternal(false) == items.get());
    items->remove(child[1].get());
    CHECK("remove", items->size() == 3 && (*items)[1] == child[2].get());
    items->remove_if([&](CDependencyObject *obj) { return obj == child[0].get(); });
    CHECK("remove_if", items->size() == 2 && (*items)[0] != child[0].get());
    items->clear();
    CHECK("clear", items->size() == 0);

    using DirectUI::Components::CollectionMoveView;
    CHECK("move arguments", CollectionMoveView::IsValid(4, 0, 3, 1, 1) && !CollectionMoveView::IsValid(4, 3, 0, 2, 2) &&
          !CollectionMoveView::HasValidArguments(0, 0, 0, 0));
    /* items 1-2 of 0,1,2,3 moved to 0 (now 1,2,0,3): the view shows the old
     * order, by where each item is now, until the move is replayed */
    CollectionMoveView view(4, 1, 0, 2);
    CHECK("move view", view.GetSize() == 4 && view.GetSourceIndex(0) == 2 && view.GetSourceIndex(1) == 0 &&
          view.GetSourceIndex(3) == 3);
    view.RemoveNext();
    view.RemoveNext();
    CHECK("move view after removing", view.GetSize() == 2);
    view.InsertNext();
    view.InsertNext();
    CHECK("move view after inserting", view.GetSize() == 4 && view.GetSourceIndex(0) == 0 && view.GetSourceIndex(3) == 3);
}

int main(void)
{
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) printf("RoInitialize failed\n");
    test_wrl();
    test_components();
    /* the lock over XAML's process-wide state (the metadata's custom types),
     * which Microsoft.UI.Xaml.dll's DllMain sets up */
    CHECK("StaticLockGlobalInit", SUCCEEDED(DirectUI::StaticLockGlobalInit()));
    test_metadata();
    test_collections();
    printf("xamlbasetest: %d passed, %d failed (%d-bit)\n", pass, fail, (int)sizeof(void *) * 8);
    return fail != 0;
}
