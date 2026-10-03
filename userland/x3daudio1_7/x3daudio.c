/*
 * x3daudio1_7.dll — X3DAudio from the DirectX SDK, on FAudio's F3DAudio
 * (the same structures; 1.7's X3DAudioInitialize returns nothing)
 */

#include <windows.h>
#include "F3DAudio.h"

int _fltused = 1;

__declspec(dllexport) void __cdecl X3DAudioInitialize(UINT32 mask, float speed_of_sound, void *instance)
{
    F3DAudioInitialize(mask, speed_of_sound, instance);
}

__declspec(dllexport) void __cdecl X3DAudioCalculate(void *instance, const void *listener, const void *emitter,
                                                     UINT32 flags, void *dsp)
{
    F3DAudioCalculate(instance, listener, emitter, flags, dsp);
}
