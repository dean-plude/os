/* NovaOS: the Windows App SDK's CompHelper/CompositionVisualSurfaceHelper.h
 * (shipped closed).  XAML sets what ICompositionVisualSurface has no
 * public property for through it: the size a visual surface is realized
 * at (connected animations, drop shadows), its stretch, and freezing it on
 * its current contents.  Declared here as XAML calls it; NovaOS defines it
 * on its Windows.UI.Composition when the composition tree builds. */
#pragma once
#include <windows.h>
#include <microsoft.ui.composition.h>

namespace CompositionVisualSurfaceHelper
{
    _Check_return_ HRESULT SetSurfaceRealizationSize(_In_ ABI::Microsoft::UI::Composition::ICompositionVisualSurface *surface,
                                                     ABI::Windows::Foundation::Numerics::Vector2 size);
    _Check_return_ HRESULT SetSurfaceStretch(_In_ ABI::Microsoft::UI::Composition::ICompositionVisualSurface *surface,
                                             ABI::Microsoft::UI::Composition::CompositionStretch stretch);
    _Check_return_ HRESULT SetSurfaceFreeze(_In_ ABI::Microsoft::UI::Composition::ICompositionVisualSurface *surface);
}
