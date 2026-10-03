# winmm.dll's build hooks: midi.c compiles TinySoundFont (third_party/tinysoundfont)
import os

TSF = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                   'third_party', 'tinysoundfont')


def cflags(b):
    return ['-I', TSF]
