// NovaOS: XAML's CValueStores.cpp (components/CValue), compiled as MSVC
// compiles it.  It explicitly instantiates its RefCounted and String value
// stores before it defines their members; MSVC instantiates the members
// defined later in the file too, clang (as the C++ standard says) only those
// defined at that point, so it would emit none of them.
// Instantiating each member explicitly after the definitions has clang
// emit them.
#include "../../../third_party/microsoft-ui-xaml/dxaml/xcp/components/CValue/CValueStores.cpp"

#define NOVA_STORE_MEMBERS(...)                                         \
    template __VA_ARGS__::MappedType __VA_ARGS__::Get(const CValue &);  \
    template void __VA_ARGS__::Copy(CValue &, const CValue &);          \
    template void __VA_ARGS__::Move(CValue &, CValue &&);               \
    template bool __VA_ARGS__::Compare(const CValue &, const CValue &); \
    template void __VA_ARGS__::Destroy(CValue &);
#define NOVA_REFCOUNTED_MEMBERS(...)                                    \
    NOVA_STORE_MEMBERS(__VA_ARGS__)                                     \
    template void __VA_ARGS__::Set(CValue &, __VA_ARGS__::MappedType);  \
    template void __VA_ARGS__::AddRef(CValue &);                        \
    template void __VA_ARGS__::Release(CValue &);

namespace CValueDetails
{
    NOVA_REFCOUNTED_MEMBERS(ValueStores::RefCounted<valueObject, CDependencyObject>)
    NOVA_REFCOUNTED_MEMBERS(ValueStores::RefCounted<valueIUnknown, IUnknown>)
    NOVA_REFCOUNTED_MEMBERS(ValueStores::RefCounted<valueIInspectable, IInspectable>)
    NOVA_REFCOUNTED_MEMBERS(ValueStores::RefCounted<valueThemeResource, CThemeResource>)
    NOVA_REFCOUNTED_MEMBERS(ValueStores::RefCounted<valueVO, Flyweight::PropertyValueObjectBase>)
    NOVA_STORE_MEMBERS(ValueStores::String<valueString>)
    template void ValueStores::String<valueString>::Set(CValue &, xstring_ptr &);
    template void ValueStores::String<valueString>::MoveXString(CValue &, xstring_ptr &&);
}
