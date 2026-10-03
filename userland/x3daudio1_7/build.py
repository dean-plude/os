# x3daudio1_7.dll: FAudio's F3DAudio (third_party/faudio)
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'xaudio2'))
import faudio_build


def cflags(b):
    return faudio_build.cflags(7)


def objs(b, odir):
    return faudio_build.objs(b, odir, 'x3daudio1_7', ['F3DAudio.c'])
