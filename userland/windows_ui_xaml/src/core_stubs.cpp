// NovaOS: stand-ins, in the manner of Microsoft's isolated-test stubs
// (components/unittests/stubs), for the members of the XAML core's element
// classes the component libraries call before the core builds:
// CDependencyObject's property system asks an element for its logical and
// templated parents and its popup, the resource lookup for an element's
// class name and resources; and of the resource lookup's, which the
// components' theme resources call (components/resources, which needs the
// XAML parser).  Linked only by programs that test the
// components on their own (xaml_teststubs.lib); none of them makes an
// element.  Microsoft's UIElement_Stub.cpp has OnAssociationFailure but
// needs the rest of the element tree with it.  Also XAML's zero-filling
// operator new (components/allocation/lib/XcpNewDelete.cpp, which also
// replaces the module's global new and joins with Windows.UI.Xaml.dll).
#include "theming\inc\Theme.h"
#include <Framework.h>
#include <Popup.h>
#include <string.h>
#include <ResourceLookupLogger.h>
#include <ResourceResolver.h>

xstring_ptr CFrameworkElement::GetClassName()
{
    return xstring_ptr::NullString();
}

CDependencyObject *CFrameworkElement::GetLogicalParentNoRef()
{
    return nullptr;
}

CDependencyObject *CFrameworkElement::GetTemplatedParent()
{
    return nullptr;
}

CResourceDictionary *CFrameworkElement::GetResourcesNoCreateNoRef() const
{
    return nullptr;
}

CPopup *CPopupRoot::GetOpenPopupWithChild(_In_ const CUIElement *const, bool) const
{
    return nullptr;
}

_Check_return_ HRESULT CUIElement::OnAssociationFailure()
{
    return E_NOTIMPL;
}

_Check_return_ HRESULT Diagnostics::ResourceLookupLogger::Start(const xstring_ptr_view &, const xstring_ptr_view &)
{
    return S_OK;
}

_Check_return_ HRESULT Diagnostics::ResourceLookupLogger::Stop(const xstring_ptr_view &, xstring_ptr &)
{
    return S_OK;
}

_Check_return_ HRESULT Resources::ResourceResolver::FindNextResolvedValueNoRef(
    _In_ CDependencyObject *, _In_ CThemeResource *, bool, _Out_ CDependencyObject **resultNoRef)
{
    *resultNoRef = nullptr;
    return S_OK;
}

void *__cdecl operator new(size_t size, ZeroMemAllocationPolicy)
{
    return memset(::operator new(size), 0, size);
}

void *__cdecl operator new[](size_t size, ZeroMemAllocationPolicy)
{
    return memset(::operator new[](size), 0, size);
}
