# vcruntime140_1.dll's build hooks (tools/build_userland.py calls them with
# itself as b).  The FH4 C++ exception handler and its helpers live in
# vcruntime140.dll; this DLL forwards to them, as Microsoft's does.  x64
# only (dll.json: x64_only).


def link(b, odir, objs, deps, base):
    b.link_dll(odir, 'vcruntime140_1', objs, deps, base,
               [f'/export:{n}=vcruntime140.{n}' for n in ('__CxxFrameHandler4', '__NLG_Dispatch2', '__NLG_Return2')])
