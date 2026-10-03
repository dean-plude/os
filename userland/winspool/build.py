# winspool's build hook (tools/build_userland.py calls it with itself as
# b): the spooler client is winspool.drv, not winspool.dll, and programs
# import it by that name
import os


def link(b, odir, objs, deps, base):
    b.link_dll(odir, 'winspool', objs, deps, base)
    path, dll = b.built[-1]
    drv = dll[:-4] + '.drv'
    os.replace(dll, drv)
    b.built[-1] = (path[:-4] + '.drv', drv)
