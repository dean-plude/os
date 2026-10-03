# msvcrt.dll's build hooks (tools/build_userland.py calls them with itself
# as b).  The C runtime links twice: as msvcrt.dll (legacy behaviour, and
# the C++ runtime the old msvcrt carried) and as ucrtbase.dll, the
# Universal C Runtime programs reach through the api-ms-win-crt-* API sets.
import os

UCRT_BASE = 0x7FFA28000000
UCRT_BASE_X86 = 0x5F000000
CXX_FROM_VCRUNTIME = ['_CxxThrowException', '__CxxFrameHandler', '__CxxFrameHandler2', '__CxxFrameHandler3',
                      '_purecall', '__RTDynamicCast', '__RTtypeid', '__RTCastToVoid', 'set_unexpected', 'unexpected',
                      '__uncaught_exception', '_set_se_translator', '_is_exception_typeof',
                      '__DestructExceptionObject', '__AdjustPointer']


def flavor_obj(b, odir, legacy):
    """msvcrt.dll and ucrtbase.dll share the C runtime's objects; this one
    differs: legacy msvcrt behaviour (e.g. printf rounding) or the UCRT's"""
    src = os.path.join(odir, 'crt_flavor.c')
    obj = os.path.join(odir, f'crt_flavor{legacy}.obj')
    open(src, 'w').write('const int __nova_crt_legacy = %d;\n' % legacy)
    b.cc(src, obj)
    return obj


def link(b, odir, objs, deps, base):
    x64 = b.ARCH == 'x64'
    math_objs, math_names = b.musl_math_objs(odir)
    objs = objs + math_objs
    rsp = os.path.join(odir, 'crt_exports.rsp')
    fwd = ['/export:__C_specific_handler=ntdll.__C_specific_handler'] if x64 else \
          [f'/export:{n}=ntdll.{n}' for n in ('_except_handler2', '_except_handler3', '_except_handler4_common',
                                              '_global_unwind2', '_local_unwind2', '_local_unwind4')]
    open(rsp, 'w').write('\n'.join([f'/export:{n}' for n in math_names] + fwd))
    extra = ['@' + rsp]
    # the old msvcrt.dll also carried the C++ runtime (7-Zip and other
    # programs built against it import exceptions and RTTI from it)
    rsp2 = os.path.join(odir, 'msvcrt_cxx.rsp')
    cxx = CXX_FROM_VCRUNTIME + (['??1type_info@@UEAA@XZ', '??_7type_info@@6B@', '_local_unwind']
                                if x64 else ['??1type_info@@UAE@XZ', '??_7type_info@@6B@'])
    open(rsp2, 'w').write('\n'.join([f'/export:{n}=vcruntime140.{n}' for n in cxx] +
                                    ['/export:?terminate@@YAXXZ=terminate']))
    b.link_dll(odir, 'msvcrt', objs + [flavor_obj(b, odir, 1)], deps, base, extra + ['@' + rsp2])
    # the Universal C Runtime: the same C runtime under its Windows 10 name
    b.link_dll(odir, 'ucrtbase', objs + [flavor_obj(b, odir, 0)], deps,
               UCRT_BASE if x64 else UCRT_BASE_X86, extra)
