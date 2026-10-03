# FAudio

The XAudio2 engine under NovaOS's `xaudio2_9.dll`, `xaudio2_8.dll`,
`xaudio2_7.dll` and `x3daudio1_7.dll` (`userland/xaudio2`): voices, mixing,
resampling, filters, the built-in effects (reverb, volume meter, FAPOFX) and
F3DAudio.

Version: **FAudio 26.07**, from Ubuntu's source package
(`faudio_26.07+dfsg.orig.tar.xz`).  Licensed under the zlib licence (see
`LICENSE`).  Kept: the engine, effect and F3DAudio sources and their
headers; left out: the SDL and Windows (WASAPI, Media Foundation) platform
layers, FACT (XACT) and the XNA song player.  NovaOS supplies its own
platform layer, `userland/xaudio2/faudio_nova.c`, which mixes into a
kernel audio stream.

Altered (marked "NovaOS" in the source): `FAudio.h` and `F3DAudio.h` let
the build define `FAUDIOAPI` and `F3DAUDIOAPI`, so the DLLs export only
the XAudio2 and X3DAudio functions.
