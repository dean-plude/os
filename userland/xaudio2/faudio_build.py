# Build helpers shared by the XAudio2 DLLs (userland/xaudio2_7, _8, _9 and
# x3daudio1_7): each compiles the sources in userland/xaudio2 with its own
# XAUDIO2_VER and links its own copy of FAudio (third_party/faudio).
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FA = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'third_party', 'faudio')
ENGINE = ['FAudio.c', 'FAudio_internal.c', 'FAudio_internal_simd.c', 'FAudio_operationset.c', 'FAPOBase.c',
          'FAudioFX_collector.c', 'FAudioFX_reverb.c', 'FAudioFX_volumemeter.c', 'FAPOFX.c', 'FAPOFX_echo.c',
          'FAPOFX_eq.c', 'FAPOFX_masteringlimiter.c', 'FAPOFX_reverb.c', 'F3DAudio.c']
# FAudio's Windows build settings (the C library for its allocator and
# maths), without exporting its own API
FLAGS = ['-DFAUDIO_WIN32_PLATFORM', '-DFAUDIOAPI=', '-DF3DAUDIOAPI=', '-I', os.path.join(FA, 'include'),
         '-I', os.path.join(FA, 'src')]


def cflags(version):
    return FLAGS + [f'-DXAUDIO2_VER={version}']


def objs(b, odir, name, srcs=ENGINE):
    headers = [os.path.join(FA, 'include', h) for h in os.listdir(os.path.join(FA, 'include'))] + \
              [os.path.join(FA, 'src', 'FAudio_internal.h')]
    return b.compile_many([os.path.join(FA, 'src', s) for s in srcs], odir, name + '_fa_',
                          b.cflags() + FLAGS + ['-w'], headers)
