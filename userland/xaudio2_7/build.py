# xaudio2_7.dll: userland/xaudio2 built as XAudio2 2.7 on FAudio
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'xaudio2'))
import faudio_build


def cflags(b):
    return faudio_build.cflags(7)


def objs(b, odir):
    return faudio_build.objs(b, odir, 'xaudio2_7')
