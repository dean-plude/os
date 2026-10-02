/*
 * vcruntime140_1.dll — the x64 C++ runtime's second half, which programs
 * built with Visual Studio 2019 and later import __CxxFrameHandler4 (the
 * frame handler for the compressed FH4 exception tables) from.
 *
 * Everything lives in vcruntime140.dll (vcruntime140/eh.c), which keeps the
 * per-thread exception state both handlers share; this DLL only forwards
 * (tools/build_userland.py links it with /export:NAME=vcruntime140.NAME).
 */
