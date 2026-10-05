# crtdll.dll's build hooks (tools/build_userland.py calls them with itself as
# b): every name in forwards.txt is exported as a forwarder to msvcrt.dll.
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def link(b, odir, objs, deps, base):
    exports = []
    for line in open(os.path.join(HERE, 'forwards.txt')):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, _, target = line.partition('=')
        exports.append(f'/export:{name}=msvcrt.{target or name}')
    rsp = os.path.join(odir, 'crtdll_exports.rsp')
    open(rsp, 'w').write('\n'.join(exports))
    b.link_dll(odir, 'crtdll', objs, deps, base, ['@' + rsp])
