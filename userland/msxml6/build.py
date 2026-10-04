# msxml6.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  libxml2 (third_party/libxml2, MIT) is compiled into the DLL: the
# parser, tree, XPath and serializer, without HTTP, FTP, catalogs, schemas
# or iconv (libxml/config.h and include/libxml/xmlversion.h set that).
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LIBXML = os.path.join(ROOT, 'third_party', 'libxml2')
SRCS = ['SAX2', 'buf', 'chvalid', 'dict', 'encoding', 'entities', 'error', 'globals', 'hash', 'list', 'parser',
        'parserInternals', 'threads', 'tree', 'uri', 'valid', 'xmlIO', 'xmlmemory', 'xmlsave', 'xmlstring', 'xpath']
INCLUDES = ['-I', os.path.join(HERE, 'libxml'), '-I', os.path.join(LIBXML, 'include'), '-DLIBXML_STATIC', '-DLIBXML_STATIC_FOR_DLL']


def cflags(b):
    """flags for msxml6's own sources"""
    return INCLUDES


def objs(b, odir):
    """libxml2 for this architecture"""
    flags = b.cflags() + INCLUDES + ['-DHAVE_CONFIG_H', '-w']
    headers = [os.path.join(HERE, 'libxml', h) for h in os.listdir(os.path.join(HERE, 'libxml'))]
    headers += [os.path.join(LIBXML, 'include', 'libxml', h) for h in os.listdir(os.path.join(LIBXML, 'include', 'libxml'))]
    return b.compile_many([os.path.join(LIBXML, s + '.c') for s in SRCS], odir, 'libxml_', flags, headers)
