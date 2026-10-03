# usp10.dll's build hooks: Uniscribe shapes with HarfBuzz from novatext.dll,
# so it compiles against HarfBuzz's headers
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def cflags(b):
    return ['-I', os.path.join(ROOT, 'third_party', 'harfbuzz', 'src')]
