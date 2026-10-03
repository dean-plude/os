# winspool's build hook (tools/build_userland.py calls it with itself as b):
# Windows names the print spooler's client winspool.drv, and programs import
# it by that name, so the DLL is built and installed under it.
import os, shutil


def link(b, odir, objs, deps, base):
    b.link_dll(odir, 'winspool', objs, deps, base)
    path, dll = b.built[-1]
    drv = os.path.join(odir, 'winspool.drv')
    shutil.copyfile(dll, drv)
    b.built[-1] = (path[:-len('.dll')] + '.drv', drv)
