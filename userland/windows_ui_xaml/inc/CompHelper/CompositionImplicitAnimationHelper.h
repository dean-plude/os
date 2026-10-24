/* NovaOS: the Windows App SDK's CompHelper/CompositionImplicitAnimationHelper.h
 * (shipped closed with the lifted composition code).  XAML's composition
 * nodes (components/comptree's HWCompTreeNodeWinRT::ImplicitAnimationDisabler)
 * turn a visual's implicit animations off while they change its properties
 * themselves and back on after.  NovaOS's compositor (dcomp.dll) runs no
 * implicit animations, so there is nothing to turn off: both succeed. */
#pragma once
#include <windows.h>
#include <microsoft.ui.composition.h>

class CompositionImplicitAnimationHelper
{
public:
    _Check_return_ HRESULT EnableImplicitAnimations(_In_ ABI::Microsoft::UI::Composition::ICompositionObject *object,
                                                    BOOL enable)
    {
        (void)object;
        (void)enable;
        return S_OK;
    }
};
