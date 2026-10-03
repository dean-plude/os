#!/usr/bin/env python3
"""gen_keyboards.py — build userland/include/kbdlayouts.h, NovaOS's keyboard layouts

One table per layout: for each typing key (set-1 scan code) the Windows
virtual-key code it sends and the characters it types plain, with Shift,
with AltGr and with Shift+AltGr, which keys are dead keys (accents typed
before a letter), and what each accent makes of each letter.  The kernel
(kernel/wm/kbdlayout.c: the desktop's own apps and the Terminal) and
user32 (ToUnicode, MapVirtualKey, VkKeyScan...) both read this header.

Where the data comes from: xkeyboard-config (the X.Org keyboard data,
MIT/X11 licence), compiled by libxkbcommon (MIT) through ctypes:

    sudo apt-get install xkb-data libxkbcommon0
    python3 tools/gen_keyboards.py

The output names the xkeyboard-config version in its header.  Virtual-key
codes are not in that data: letters send their own (VK_A-VK_Z, so German
Z and Y swap theirs as on Windows), the digit row sends VK_0-VK_9, the
keys typing + , - . send VK_OEM_PLUS/COMMA/MINUS/PERIOD (Windows'
European layouts do the same) and every other key keeps the US
layout's code for its place.
"""

import ctypes
import os
import subprocess
import sys
import unicodedata

OUT = os.path.join(os.path.dirname(__file__), '..', 'userland', 'include', 'kbdlayouts.h')

# Windows layout id (KLID), input locale handle (HKL), name, xkb layout and variant
LAYOUTS = [
    ('00000409', 0x04090409, 'English (United States)', 'us', ''),
    ('00000809', 0x08090809, 'English (United Kingdom)', 'gb', ''),
    ('00010409', 0xF0020409, 'English (United States, Dvorak)', 'us', 'dvorak'),
    ('00000407', 0x04070407, 'German', 'de', ''),
    ('00000807', 0x08070807, 'German (Switzerland)', 'ch', ''),
    ('0000040C', 0x040C040C, 'French', 'fr', ''),
    ('0000100C', 0x100C100C, 'French (Switzerland)', 'ch', 'fr'),
    ('00001009', 0x10090C0C, 'French (Canada)', 'ca', ''),
    ('0000040A', 0x040A040A, 'Spanish', 'es', ''),
    ('00000410', 0x04100410, 'Italian', 'it', ''),
    ('00000816', 0x08160816, 'Portuguese', 'pt', ''),
    ('00000416', 0x04160416, 'Portuguese (Brazil)', 'br', ''),
    ('0000041D', 0x041D041D, 'Swedish', 'se', ''),
    ('0000040B', 0x040B040B, 'Finnish', 'fi', 'classic'),
    ('00000414', 0x04140414, 'Norwegian', 'no', ''),
    ('00000406', 0x04060406, 'Danish', 'dk', ''),
]

# The typing keys: set-1 scan codes (evdev keycode = scan code + 8; the
# Brazilian ABNT2 key /? is Linux KEY_RO = 89, set-1 0x73)
KEYS = (list(range(0x02, 0x0E)) + list(range(0x10, 0x1C)) + list(range(0x1E, 0x2A)) +
        list(range(0x2B, 0x36)) + [0x56, 0x73])


def evdev(sc):
    return 89 + 8 if sc == 0x73 else sc + 8


# The US layout's virtual-key codes by place (as kernel/um/um_gui.c's table)
US_VK = {
    0x02: '1', 0x03: '2', 0x04: '3', 0x05: '4', 0x06: '5', 0x07: '6', 0x08: '7', 0x09: '8', 0x0A: '9',
    0x0B: '0', 0x0C: 0xBD, 0x0D: 0xBB, 0x10: 'Q', 0x11: 'W', 0x12: 'E', 0x13: 'R', 0x14: 'T', 0x15: 'Y',
    0x16: 'U', 0x17: 'I', 0x18: 'O', 0x19: 'P', 0x1A: 0xDB, 0x1B: 0xDD, 0x1E: 'A', 0x1F: 'S', 0x20: 'D',
    0x21: 'F', 0x22: 'G', 0x23: 'H', 0x24: 'J', 0x25: 'K', 0x26: 'L', 0x27: 0xBA, 0x28: 0xDE, 0x29: 0xC0,
    0x2B: 0xDC, 0x2C: 'Z', 0x2D: 'X', 0x2E: 'C', 0x2F: 'V', 0x30: 'B', 0x31: 'N', 0x32: 'M', 0x33: 0xBC,
    0x34: 0xBE, 0x35: 0xBF, 0x56: 0xE2, 0x73: 0xC1,
}

# xkb dead keys: the spacing character Windows shows for each, and the
# combining mark it puts on a letter
DEAD = {
    'dead_grave': (0x60, 0x300), 'dead_acute': (0xB4, 0x301), 'dead_circumflex': (0x5E, 0x302),
    'dead_tilde': (0x7E, 0x303), 'dead_macron': (0xAF, 0x304), 'dead_breve': (0x2D8, 0x306),
    'dead_abovedot': (0x2D9, 0x307), 'dead_diaeresis': (0xA8, 0x308), 'dead_abovering': (0x2DA, 0x30A),
    'dead_doubleacute': (0x2DD, 0x30B), 'dead_caron': (0x2C7, 0x30C), 'dead_cedilla': (0xB8, 0x327),
    'dead_ogonek': (0x2DB, 0x328),
}


class RuleNames(ctypes.Structure):
    _fields_ = [(n, ctypes.c_char_p) for n in ('rules', 'model', 'layout', 'variant', 'options')]


def load_xkb():
    x = ctypes.CDLL('libxkbcommon.so.0')
    x.xkb_context_new.restype = ctypes.c_void_p
    x.xkb_context_new.argtypes = [ctypes.c_int]
    x.xkb_keymap_new_from_names.restype = ctypes.c_void_p
    x.xkb_keymap_new_from_names.argtypes = [ctypes.c_void_p, ctypes.POINTER(RuleNames), ctypes.c_int]
    x.xkb_keymap_key_get_syms_by_level.restype = ctypes.c_int
    x.xkb_keymap_key_get_syms_by_level.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                                   ctypes.c_uint32, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32))]
    x.xkb_keysym_to_utf32.restype = ctypes.c_uint32
    x.xkb_keysym_to_utf32.argtypes = [ctypes.c_uint32]
    x.xkb_keysym_get_name.restype = ctypes.c_int
    x.xkb_keysym_get_name.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
    return x


def xkb_version():
    try:
        out = subprocess.run(['dpkg-query', '-W', '-f=${Version}', 'xkb-data'], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout:
            return out.stdout.split('-')[0]
    except OSError:
        pass
    return 'unknown'


def layout_keys(x, ctx, layout, variant):
    names = RuleNames(b'evdev', b'pc105', layout.encode(), variant.encode(), b'')
    km = x.xkb_keymap_new_from_names(ctx, ctypes.byref(names), 0)
    if not km:
        sys.exit(f'gen_keyboards: xkb has no layout {layout}({variant})')
    keys, deads = [], set()
    for sc in KEYS:
        chars, dead = [], 0
        for level in range(4):
            syms = ctypes.POINTER(ctypes.c_uint32)()
            n = x.xkb_keymap_key_get_syms_by_level(km, evdev(sc), 0, level, ctypes.byref(syms))
            c = 0
            if n == 1:
                buf = ctypes.create_string_buffer(64)
                x.xkb_keysym_get_name(syms[0], buf, 64)
                name = buf.value.decode()
                if name in DEAD:
                    c = DEAD[name][0]
                    dead |= 1 << level
                    deads.add(name)
                elif not name.startswith('dead_'):
                    c = x.xkb_keysym_to_utf32(syms[0])
                    if c > 0xFFFF or c < 0x20 or c == 0x7F:
                        c = 0
            chars.append(c)
        if sc == 0x73 and not any(chars):
            continue                                    # (only ABNT2 keyboards have it)
        if sc == 0x56 and layout == 'us':
            chars = [ord('\\'), ord('|'), 0, 0]       # (Windows' US layouts: \ and | there)
        keys.append((sc, chars, dead))
    return keys, deads


def assign_vks(keys, us_chars):
    """Virtual-key codes for one layout's keys: letters and digits their
    own, a punctuation key the code of the US key typing the same
    character, the rest their place's US code, or a free VK_OEM_* code
    when another key took that"""
    vks, used = {}, set()
    for sc, chars, _ in keys:
        b = chars[0]
        v = None
        if ord('a') <= b <= ord('z'):
            v = b - 32
        elif 0x02 <= sc <= 0x0B:
            v = ord('1') + sc - 0x02 if sc < 0x0B else ord('0')
        elif b in us_chars and not (chr(b).isascii() and chr(b).isalnum()):
            v = us_chars[b]
        if v is not None and v not in used:
            vks[sc] = v
            used.add(v)
    pool = [0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE2, 0xC1]
    for sc, chars, _ in keys:
        if sc in vks:
            continue
        v = US_VK[sc]
        v = ord(v) if isinstance(v, str) else v
        if chr(v).isascii() and chr(v).isalnum() or v in used:
            v = next(o for o in pool if o not in used)
        vks[sc] = v
        used.add(v)
    return vks


def caps(chars):
    a, b = chars[0], chars[1]
    return bool(a and b and chr(a).isalpha() and chr(a).upper() == chr(b) and chr(a) != chr(b))


def compose_table(all_deads):
    rows = []
    for name in sorted(all_deads, key=lambda n: DEAD[n][0]):
        spacing, mark = DEAD[name]
        for base in [chr(c) for c in range(ord('A'), ord('Z') + 1)] + [chr(c) for c in range(ord('a'), ord('z') + 1)]:
            out = unicodedata.normalize('NFC', base + chr(mark))
            if len(out) == 1 and ord(out) <= 0xFFFF:
                rows.append((spacing, ord(base), ord(out)))
        rows.append((spacing, 0x20, spacing))
    return rows


def c_char(c):
    return f'0x{c:04X}'


def main():
    x = load_xkb()
    ctx = x.xkb_context_new(0)
    out, all_deads = [], set()
    tables = []
    for klid, hkl, name, layout, variant in LAYOUTS:
        keys, deads = layout_keys(x, ctx, layout, variant)
        all_deads |= deads
        tables.append((klid, hkl, name, layout, variant, keys))

    out.append('/*')
    out.append(' * kbdlayouts.h — GENERATED by tools/gen_keyboards.py; do not edit by hand.')
    out.append(' *')
    out.append(f' * Keyboard layouts from xkeyboard-config {xkb_version()} (MIT/X11 licence),')
    out.append(' * compiled by libxkbcommon.  Shared by the kernel (wm/kbdlayout.c) and')
    out.append(' * user32 (kbd.c); tools/gen_keyboards.py says how they are made.')
    out.append(' */')
    out.append('#pragma once')
    out.append('')
    out.append('/* KbdKey.flags: Caps Lock shifts the key (letters); bits 4-7: the level')
    out.append(' * (plain, Shift, AltGr, Shift+AltGr) is a dead key */')
    out.append('#define KBD_CAPS 0x01')
    out.append('#define KBD_DEAD(level) (0x10 << (level))')
    out.append('')
    out.append('typedef struct { unsigned char sc, vk, flags; unsigned short ch[4]; } KbdKey;')
    out.append('typedef struct {')
    out.append('    const char *klid;          /* "00000407": the layout id, as Windows names it */')
    out.append('    unsigned int hkl;          /* its input locale handle (GetKeyboardLayout) */')
    out.append('    const char *name;          /* "German" */')
    out.append('    const KbdKey *keys;')
    out.append('    int nkeys;')
    out.append('    int altgr;                 /* it has AltGr characters (right Alt is AltGr) */')
    out.append('} KbdLayout;')
    out.append('typedef struct { unsigned short dead, base, out; } KbdCompose;')
    out.append('')
    us_keys = tables[0][5]
    us_vks, us_chars = assign_vks(us_keys, {}), {}
    for sc, chars, _ in us_keys:
        us_chars.setdefault(chars[0], us_vks[sc])
    for i, (klid, hkl, name, layout, variant, keys) in enumerate(tables):
        vks = assign_vks(keys, us_chars)
        src = layout + (f'({variant})' if variant else '')
        out.append(f'static const KbdKey kbd_keys_{klid}[] = {{   /* xkb {src} */')
        for sc, chars, dead in keys:
            flags = (1 if caps(chars) else 0) | (dead << 4)
            out.append(f'    {{ 0x{sc:02X}, 0x{vks[sc]:02X}, 0x{flags:02X}, '
                       f'{{ {", ".join(c_char(c) for c in chars)} }} }},')
        out.append('};')
    out.append('')
    out.append('static const KbdLayout kbd_layouts[] = {')
    for klid, hkl, name, layout, variant, keys in tables:
        altgr = int(any(chars[2] or chars[3] for _, chars, _ in keys))
        out.append(f'    {{ "{klid}", 0x{hkl:08X}u, "{name}", kbd_keys_{klid}, '
                   f'(int)(sizeof(kbd_keys_{klid}) / sizeof(KbdKey)), {altgr} }},')
    out.append('};')
    out.append('#define KBD_NLAYOUTS ((int)(sizeof(kbd_layouts) / sizeof(kbd_layouts[0])))')
    out.append('')
    out.append('/* A dead key\'s accent on a letter (and on a space: the accent itself) */')
    out.append('static const KbdCompose kbd_compose[] = {')
    for d, b, o in compose_table(all_deads):
        out.append(f'    {{ 0x{d:04X}, 0x{b:04X}, 0x{o:04X} }},')
    out.append('};')
    out.append('#define KBD_NCOMPOSE ((int)(sizeof(kbd_compose) / sizeof(kbd_compose[0])))')
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print(f'{OUT}: {len(tables)} layouts, {len(compose_table(all_deads))} accented letters')


if __name__ == '__main__':
    main()
