#!/usr/bin/env python3
"""mkmsi.py — write Windows Installer files in pure Python: packages
(.msi), transforms (.mst) and patches (.msp), with their cabinets.

The self-tests build their packages with it (tools/msitest/mkpkg.py), so
the build needs neither Windows nor msitools.  What it writes:

  * OLE compound files (version 3, 512-byte sectors, the mini stream for
    streams under 4 KB, storages inside storages for the transforms a patch
    carries), directory entries in a red-black tree;
  * the database: !_StringPool / !_StringData, !_Tables, !_Columns and each
    table's stream, column-major; binary cells as "Table.Key" streams;
    stream names in the installer's packed 6-bit encoding;
  * the summary information property set;
  * transforms: the difference between two databases, row by row (insert,
    delete, or an update of the columns a mask names);
  * patches: a .msp holding a transform pair (":T" changes the product,
    ":#T" adds the patch's Media row) and the cabinet of new files;
  * MSZIP cabinets (one folder, blocks of 32 KB, deflate through zlib).
"""
import struct, zlib, time

# -----------------------------------------------------------------------
# Column types (the _Columns Type value)
# -----------------------------------------------------------------------
T_NULLABLE, T_KEY, T_LOCAL = 0x1000, 0x2000, 0x0200


def coltype(spec):
    """'s72' string, 'l255' localizable string, 'i2' short, 'i4' long,
    'v0' binary; upper case first letter = nullable; a trailing '*' = key"""
    key = spec.endswith('*')
    spec = spec.rstrip('*')
    kind, size = spec[0], int(spec[1:] or 0)
    t = {'s': 0x0D00 | size, 'l': 0x0F00 | size, 'i': 0x0502 if size == 2 else 0x0104, 'v': 0x0900}[kind.lower()]
    if kind.isupper():
        t |= T_NULLABLE
    if key:
        t |= T_KEY
    return t


def is_string(t):
    return bool(t & 0x0800)


def is_binary(t):
    return (t & ~(T_NULLABLE | T_KEY | 0x4000)) == 0x0900


# The usual tables: name -> [(column, spec)]
SCHEMA = {
    'Property': [('Property', 's72*'), ('Value', 'l0')],
    'Directory': [('Directory', 's72*'), ('Directory_Parent', 'S72'), ('DefaultDir', 'l255')],
    'Component': [('Component', 's72*'), ('ComponentId', 'S38'), ('Directory_', 's72'), ('Attributes', 'i2'),
                  ('Condition', 'S255'), ('KeyPath', 'S72')],
    'Feature': [('Feature', 's38*'), ('Feature_Parent', 'S38'), ('Title', 'L64'), ('Description', 'L255'),
                ('Display', 'I2'), ('Level', 'i2'), ('Directory_', 'S72'), ('Attributes', 'i2')],
    'FeatureComponents': [('Feature_', 's38*'), ('Component_', 's72*')],
    'File': [('File', 's72*'), ('Component_', 's72'), ('FileName', 'l255'), ('FileSize', 'i4'), ('Version', 'S72'),
             ('Language', 'S20'), ('Attributes', 'I2'), ('Sequence', 'i4')],
    'Media': [('DiskId', 'i2*'), ('LastSequence', 'i4'), ('DiskPrompt', 'L64'), ('Cabinet', 'S255'),
              ('VolumeLabel', 'S32'), ('Source', 'S72')],
    'Registry': [('Registry', 's72*'), ('Root', 'i2'), ('Key', 'l255'), ('Name', 'L255'), ('Value', 'L0'),
                 ('Component_', 's72')],
    'CreateFolder': [('Directory_', 's72*'), ('Component_', 's72*')],
    'Environment': [('Environment', 's72*'), ('Name', 'l255'), ('Value', 'L255'), ('Component_', 's72')],
    'Shortcut': [('Shortcut', 's72*'), ('Directory_', 's72'), ('Name', 'l128'), ('Component_', 's72'),
                 ('Target', 's72'), ('Arguments', 'S255'), ('Description', 'L255'), ('Hotkey', 'I2'),
                 ('Icon_', 'S72'), ('IconIndex', 'I2'), ('ShowCmd', 'I2'), ('WkDir', 'S72')],
    'ServiceInstall': [('ServiceInstall', 's72*'), ('Name', 's255'), ('DisplayName', 'L255'), ('ServiceType', 'i4'),
                       ('StartType', 'i4'), ('ErrorControl', 'i4'), ('LoadOrderGroup', 'S255'),
                       ('Dependencies', 'S255'), ('StartName', 'S255'), ('Password', 'S255'),
                       ('Arguments', 'S255'), ('Component_', 's72'), ('Description', 'L255')],
    'ServiceControl': [('ServiceControl', 's72*'), ('Name', 'l255'), ('Event', 'i2'), ('Arguments', 'L255'),
                       ('Wait', 'I2'), ('Component_', 's72')],
    'CustomAction': [('Action', 's72*'), ('Type', 'i2'), ('Source', 'S72'), ('Target', 'S255')],
    'Binary': [('Name', 's72*'), ('Data', 'v0')],
    'InstallExecuteSequence': [('Action', 's72*'), ('Condition', 'S255'), ('Sequence', 'I2')],
    'PatchPackage': [('PatchId', 's38*'), ('Media_', 'i2')],
    'Patch': [('File_', 's72*'), ('Sequence', 'i4*'), ('PatchSize', 'i4'), ('Attributes', 'i2'),
              ('Header', 'V0'), ('StreamRef_', 'S38')],
    'MsiFileHash': [('File_', 's72*'), ('Options', 'i2'), ('HashPart1', 'i4'), ('HashPart2', 'i4'),
                    ('HashPart3', 'i4'), ('HashPart4', 'i4')],
}

MSI_CLSID = bytes.fromhex('8410 0c00 0000 0000 c000 0000 0000 0046'.replace(' ', ''))
MST_CLSID = bytes.fromhex('8210 0c00 0000 0000 c000 0000 0000 0046'.replace(' ', ''))
MSP_CLSID = bytes.fromhex('8610 0c00 0000 0000 c000 0000 0000 0046'.replace(' ', ''))


# -----------------------------------------------------------------------
# Stream names: two characters of [0-9A-Za-z._] per code unit
# -----------------------------------------------------------------------
_CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._'


def encode_name(name, table=False):
    out = [0x4840] if table else []
    i = 0
    while i < len(name):
        c = name[i]
        if c in _CHARS:
            a = _CHARS.index(c)
            if i + 1 < len(name) and name[i + 1] in _CHARS:
                out.append(0x3800 + a + (_CHARS.index(name[i + 1]) << 6))
                i += 2
            else:
                out.append(0x4800 + a)
                i += 1
        else:
            out.append(ord(c))
            i += 1
    return ''.join(chr(c) for c in out)


# -----------------------------------------------------------------------
# Compound files
# -----------------------------------------------------------------------
ENDOFCHAIN, FREESECT, FATSECT, NOSTREAM = 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFD, 0xFFFFFFFF


class Storage:
    """A storage: name -> bytes (a stream) or Storage (raw directory names)"""
    def __init__(self, clsid=b'\0' * 16):
        self.clsid = clsid
        self.items = {}


def _cfb_key(name):
    return (len(name), name.upper())


def write_cfb(root):
    entries = []                     # [name, type, clsid, data, children(list of idx)]

    def add(name, obj):
        i = len(entries)
        if isinstance(obj, Storage):
            entries.append([name, 1, obj.clsid, None, []])
            entries[i][4] = [add(n, o) for n, o in obj.items.items()]
        else:
            entries.append([name, 2, b'\0' * 16, bytes(obj), []])
        return i

    add('Root Entry', root)
    entries[0][1] = 5
    # the mini stream: streams under 4096 bytes, 64-byte sectors
    mini = bytearray()
    minifat = []
    start = {}
    for i, e in enumerate(entries):
        if e[1] == 2 and len(e[3]) < 4096:
            if not e[3]:
                start[i] = ENDOFCHAIN
                continue
            first = len(mini) // 64
            n = (len(e[3]) + 63) // 64
            start[i] = first
            minifat += [first + k + 1 for k in range(n - 1)] + [ENDOFCHAIN]
            mini += e[3] + b'\0' * (n * 64 - len(e[3]))
    # regular sectors: big streams, the mini stream, the mini FAT, the directory
    sectors = []                     # each 512 bytes
    fat = []

    def chain(data):
        if not data:
            return ENDOFCHAIN
        first = len(sectors)
        n = (len(data) + 511) // 512
        for k in range(n):
            sectors.append(bytes(data[k * 512:(k + 1) * 512]).ljust(512, b'\0'))
            fat.append(first + k + 1 if k < n - 1 else ENDOFCHAIN)
        return first

    for i, e in enumerate(entries):
        if e[1] == 2 and len(e[3]) >= 4096:
            start[i] = chain(e[3])
    mini_start = chain(bytes(mini))
    mf = b''.join(struct.pack('<I', x) for x in minifat)
    minifat_start = chain(mf) if minifat else ENDOFCHAIN
    nminifat = (len(mf) + 511) // 512

    # the red-black trees: each storage's children by (length, upper case)
    left = [NOSTREAM] * len(entries)
    right = [NOSTREAM] * len(entries)
    child = [NOSTREAM] * len(entries)
    color = [1] * len(entries)       # black

    def build(ids, depth, maxdepth):
        if not ids:
            return NOSTREAM
        mid = len(ids) // 2
        n = ids[mid]
        color[n] = 0 if depth == maxdepth and maxdepth > 0 and not _perfect(len_all[0]) else 1
        left[n] = build(ids[:mid], depth + 1, maxdepth)
        right[n] = build(ids[mid + 1:], depth + 1, maxdepth)
        return n

    def _perfect(n):
        return (n + 1) & n == 0

    for i, e in enumerate(entries):
        if e[1] in (1, 5) and e[4]:
            ids = sorted(e[4], key=lambda k: _cfb_key(entries[k][0]))
            maxdepth = len(ids).bit_length() - 1
            len_all = [len(ids)]
            child[i] = build(ids, 0, maxdepth)
    d = bytearray()
    for i, e in enumerate(entries):
        nm = e[0].encode('utf-16-le')
        assert len(nm) <= 62, e[0]
        if e[1] == 5:
            st, size = (mini_start if mini else ENDOFCHAIN), len(mini)
        elif e[1] == 2:
            st, size = start[i], len(e[3])
        else:
            st, size = 0, 0
        d += nm.ljust(64, b'\0') + struct.pack('<HBB', len(nm) + 2, e[1], color[i])
        d += struct.pack('<III', left[i], right[i], child[i]) + e[2] + b'\0' * 4 + b'\0' * 16
        d += struct.pack('<III', st, size, 0)
    while len(d) % 512:
        d += b'\0' * 64 + struct.pack('<HBB', 0, 0, 0) + struct.pack('<III', NOSTREAM, NOSTREAM, NOSTREAM) + b'\0' * 48
    dir_start = chain(bytes(d))
    # the FAT, which covers its own sectors too
    nfat = 1
    while (len(sectors) + nfat) > nfat * 128:
        nfat += 1
    assert nfat <= 109, 'file too large for this writer'
    fat_first = len(sectors)
    fat += [FATSECT] * nfat
    fat += [FREESECT] * (nfat * 128 - len(fat))
    for k in range(nfat):
        sectors.append(b''.join(struct.pack('<I', x) for x in fat[k * 128:(k + 1) * 128]))
    hdr = bytearray(b'\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1' + b'\0' * 16)
    hdr += struct.pack('<HHHHH', 0x3E, 3, 0xFFFE, 9, 6) + b'\0' * 6
    hdr += struct.pack('<IIIIIIIII', 0, nfat, dir_start, 0, 4096, minifat_start, nminifat, ENDOFCHAIN, 0)
    difat = [fat_first + k for k in range(nfat)] + [FREESECT] * (109 - nfat)
    hdr += b''.join(struct.pack('<I', x) for x in difat)
    assert len(hdr) == 512
    return bytes(hdr) + b''.join(sectors)


# -----------------------------------------------------------------------
# Summary information
# -----------------------------------------------------------------------
PID_CODEPAGE, PID_TITLE, PID_SUBJECT, PID_AUTHOR, PID_KEYWORDS, PID_COMMENTS = 1, 2, 3, 4, 5, 6
PID_TEMPLATE, PID_LASTAUTHOR, PID_REVNUMBER, PID_CREATE_DTM, PID_LASTSAVE_DTM = 7, 8, 9, 12, 13
PID_PAGECOUNT, PID_WORDCOUNT, PID_CHARCOUNT, PID_APPNAME, PID_SECURITY = 14, 15, 16, 18, 19


def summary_info(props):
    """props: pid -> str (VT_LPSTR), int (VT_I4; the code page VT_I2)"""
    props = dict(props)
    props.setdefault(PID_CODEPAGE, 1252)
    body = b''
    index = []
    base = 8 + 8 * len(props)
    for pid in sorted(props):
        v = props[pid]
        index.append((pid, base + len(body)))
        if pid == PID_CODEPAGE:
            body += struct.pack('<IhH', 2, v, 0)
        elif isinstance(v, int):
            body += struct.pack('<Ii', 3, v)
        else:
            s = v.encode('cp1252') + b'\0'
            s += b'\0' * (-len(s) % 4)
            body += struct.pack('<II', 30, len(v) + 1) + s
    section = struct.pack('<II', base + len(body), len(props)) + b''.join(struct.pack('<II', p, o) for p, o in index) + body
    fmtid = bytes.fromhex('e0859ff2f94f6810ab9108002b27b3d9')
    head = struct.pack('<HHI', 0xFFFE, 0, 0x00020006) + b'\0' * 16 + struct.pack('<I', 1) + fmtid + struct.pack('<I', 48)
    return head + section


# -----------------------------------------------------------------------
# Databases
# -----------------------------------------------------------------------
class Table:
    def __init__(self, name, cols):
        self.name = name
        self.cols = [(c, coltype(s) if isinstance(s, str) else s) for c, s in cols]
        self.rows = []

    def keys(self):
        return [i for i, (_, t) in enumerate(self.cols) if t & T_KEY]

    def key_of(self, row):
        return tuple(row[i] for i in self.keys())


class Database:
    def __init__(self, codepage=1252):
        self.tables = {}
        self.streams = {}            # "Table.Key" -> bytes (binary cells), or other streams
        self.storages = {}           # embedded transforms etc.: name -> Storage
        self.summary = {}
        self.codepage = codepage

    def table(self, name, cols=None):
        if name not in self.tables:
            self.tables[name] = Table(name, cols if cols is not None else SCHEMA[name])
        return self.tables[name]

    def add(self, name, *rows):
        t = self.table(name)
        for r in rows:
            r = list(r)
            assert len(r) == len(t.cols), (name, r)
            for i, (c, ty) in enumerate(t.cols):
                if is_binary(ty) and isinstance(r[i], (bytes, bytearray)):
                    key = '.'.join(str(r[k]) for k in t.keys())
                    self.streams[f'{name}.{key}'] = bytes(r[i])
                    r[i] = 1
            t.rows.append(r)

    def copy(self):
        import copy
        return copy.deepcopy(self)

    def storage(self, clsid=MSI_CLSID):
        """The database as a compound-file storage"""
        pool = StringPool(self.codepage)
        st = Storage(clsid)
        names = sorted(self.tables)
        cols_rows = []
        for tn in names:
            t = self.tables[tn]
            for n, (c, ty) in enumerate(t.cols, 1):
                cols_rows.append((pool.ref(tn), n, pool.ref(c), ty))
        tables_stream = b''.join(struct.pack('<H', pool.ref(tn)) for tn in names)
        streams = {}
        for tn in names:
            t = self.tables[tn]
            rows = sorted(t.rows, key=lambda r: tuple((0, x) if isinstance(x, int) else (1, str(x)) for x in t.key_of(r)))
            data = b''
            for i, (c, ty) in enumerate(t.cols):
                for r in rows:
                    data += pool.cell(r[i], ty)
            streams[encode_name(tn, True)] = data
        cdata = b''
        for k in range(4):
            for r in cols_rows:
                v = r[k]
                cdata += pool.strref(v) if k in (0, 2) else struct.pack('<H', (v + 0x8000) & 0xFFFF)
        # (string ids are final now: the pool is written last)
        st.items[encode_name('_Tables', True)] = b''.join(pool.strref(pool.ref(tn)) for tn in names)
        st.items[encode_name('_Columns', True)] = cdata
        st.items.update(streams)
        pst, dst = pool.streams()
        st.items[encode_name('_StringPool', True)] = pst
        st.items[encode_name('_StringData', True)] = dst
        for n, data in self.streams.items():
            st.items[encode_name(n)] = data
        for n, s in self.storages.items():           # (storage names are not packed)
            st.items[n] = s
        if self.summary:
            st.items['\x05SummaryInformation'] = summary_info(self.summary)
        return st

    def write(self, path):
        open(path, 'wb').write(write_cfb(self.storage()))


class StringPool:
    def __init__(self, codepage):
        self.codepage = codepage
        self.ids = {}
        self.strings = [None]
        self.refs = [0]
        self.wide = False

    def ref(self, s):
        if s is None or s == '':
            return 0
        s = str(s)
        if s not in self.ids:
            self.ids[s] = len(self.strings)
            self.strings.append(s)
            self.refs.append(0)
        i = self.ids[s]
        self.refs[i] += 1
        return i

    def strref(self, i):
        return struct.pack('<H', i)

    def cell(self, v, ty):
        if is_binary(ty):
            return struct.pack('<H', 1 if v else 0)
        if is_string(ty):
            return self.strref(self.ref(v))
        if v is None or v == '':
            return b'\0' * (4 if (ty & 0xFF) == 4 else 2)
        if (ty & 0xFF) == 4:
            return struct.pack('<I', (int(v) + 0x80000000) & 0xFFFFFFFF)
        return struct.pack('<H', (int(v) + 0x8000) & 0xFFFF)

    def streams(self):
        assert len(self.strings) < 0x10000, 'pool too large for 2-byte references'
        pool = struct.pack('<I', self.codepage)
        data = b''
        for s, n in zip(self.strings[1:], self.refs[1:]):
            b = s.encode('cp1252' if self.codepage == 1252 else 'utf-8')
            assert len(b) < 0x10000
            pool += struct.pack('<HH', len(b), min(n, 0xFFFF))
            data += b
        return pool, data


# -----------------------------------------------------------------------
# Transforms: what turns database @a into database @b
# -----------------------------------------------------------------------
def transform(a, b, summary=None, clsid=MST_CLSID):
    pool = StringPool(b.codepage)
    st = Storage(clsid)

    def cells(t, row, cols):
        out = b''
        for i in cols:
            out += pool.cell(row[i], t.cols[i][1])
        return out

    new_tables = [n for n in sorted(b.tables) if n not in a.tables]
    gone_tables = [n for n in sorted(a.tables) if n not in b.tables]
    # _Columns: the columns of new tables; _Tables: new and dropped tables
    if new_tables:
        data = b''
        for tn in new_tables:
            for n, (c, ty) in enumerate(b.tables[tn].cols, 1):
                data += struct.pack('<H', (4 << 8) | 1) + pool.strref(pool.ref(tn)) + struct.pack('<H', (n + 0x8000) & 0xFFFF)
                data += pool.strref(pool.ref(c)) + struct.pack('<H', (ty + 0x8000) & 0xFFFF)
        st.items[encode_name('_Columns', True)] = data
    if new_tables or gone_tables:
        data = b''
        for tn in new_tables:
            data += struct.pack('<H', (1 << 8) | 1) + pool.strref(pool.ref(tn))
        for tn in gone_tables:
            data += struct.pack('<H', 0) + pool.strref(pool.ref(tn))
        st.items[encode_name('_Tables', True)] = data
    for tn in sorted(b.tables):
        tb = b.tables[tn]
        ta = a.tables.get(tn)
        keys = tb.keys()
        old = {ta.key_of(r): r for r in ta.rows} if ta else {}
        new = {tb.key_of(r): r for r in tb.rows}
        data = b''
        for k, r in old.items():
            if k not in new:                                  # deleted: keys only, mask 0
                data += struct.pack('<H', 0) + cells(tb, r, keys)
        for k, r in new.items():
            if k not in old:                                  # inserted: every column
                data += struct.pack('<H', (len(tb.cols) << 8) | 1) + cells(tb, r, range(len(tb.cols)))
            else:
                o = old[k]
                changed = [i for i in range(len(tb.cols)) if i not in keys and _norm(o[i]) != _norm(r[i])]
                if not changed:
                    continue
                assert max(changed) < 16, 'update masks name 16 columns'
                mask = 0
                for i in changed:
                    mask |= 1 << i
                data += struct.pack('<H', mask) + cells(tb, r, sorted(set(keys) | set(changed)))
        if data:
            st.items[encode_name(tn, True)] = data
    for n, d in b.streams.items():
        if a.streams.get(n) != d:
            st.items[encode_name(n)] = d
    pst, dst = pool.streams()
    st.items[encode_name('_StringPool', True)] = pst
    st.items[encode_name('_StringData', True)] = dst
    st.items['\x05SummaryInformation'] = summary_info(summary or {})
    return st


def _norm(v):
    return '' if v is None else str(v)


def transform_summary(a, b, validate=0x0002 << 16):
    """A transform's summary: the products before and after, and what the
    installer checks before applying it (default: the product code)"""
    pa = {r[0]: r[1] for r in a.tables['Property'].rows}
    pb = {r[0]: r[1] for r in b.tables['Property'].rows}
    return {PID_TITLE: 'Transform', PID_TEMPLATE: ';1033', PID_LASTAUTHOR: ';1033',
            PID_REVNUMBER: f"{pa['ProductCode']}{pa['ProductVersion']};{pb['ProductCode']}{pb['ProductVersion']};{pa.get('UpgradeCode', '')}",
            PID_PAGECOUNT: 200, PID_CHARCOUNT: validate}


def write_transform(a, b, path, validate=0x0002 << 16):
    open(path, 'wb').write(write_cfb(transform(a, b, transform_summary(a, b, validate))))


# -----------------------------------------------------------------------
# Cabinets
# -----------------------------------------------------------------------
def cabinet(files, compress=True):
    """files: [(name, bytes)] -> an MSZIP (or stored) cabinet with one folder"""
    blob = b''.join(d for _, d in files)
    blocks = []
    for k in range(0, max(len(blob), 1), 32768):
        chunk = blob[k:k + 32768]
        if compress:
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = b'CK' + co.compress(chunk) + co.flush()
        else:
            comp = chunk
        blocks.append((comp, len(chunk)))
    fentries = b''
    off = 0
    for name, d in files:
        fentries += struct.pack('<IIHHHH', len(d), off, 0, 0x5821, 0, 0x20) + name.encode('ascii') + b'\0'
        off += len(d)
    header_len = 36
    folder_len = 8
    data_off = header_len + folder_len + len(fentries)
    total = data_off + sum(8 + len(c) for c, _ in blocks)
    out = b'MSCF' + struct.pack('<IIIIIBBHHHHH', 0, total, 0, header_len + folder_len, 0, 3, 1, 1, len(files), 0, 0x4E56, 0)
    out += struct.pack('<IHH', data_off, len(blocks), 1 if compress else 0)
    out += fentries
    for c, n in blocks:
        head = struct.pack('<HH', len(c), n)
        out += struct.pack('<I', _cab_csum(head, _cab_csum(c, 0))) + head + c
    assert len(out) == total
    return out


def _cab_csum(data, seed):
    """The CFDATA checksum: the data XORed as little-endian words, the
    leftover bytes taken first-byte-highest"""
    csum = seed
    n = len(data) // 4
    for (w,) in struct.iter_unpack('<I', data[:n * 4]):
        csum ^= w
    ul = 0
    for b in data[n * 4:]:
        ul = (ul << 8) | b
    return csum ^ ul


# -----------------------------------------------------------------------
# Patches
# -----------------------------------------------------------------------
def patch(target, upgraded, patch_code, files, path, name='NovaPatch', disk=100):
    """A .msp updating @target (a Database) to @upgraded: @files are the
    File keys whose contents ship whole in the patch's cabinet ({key: bytes});
    their File rows in @upgraded get sequence numbers after the target's."""
    up = upgraded.copy()
    ft = up.tables['File']
    seq_col = [c for c, _ in ft.cols].index('Sequence')
    size_col = [c for c, _ in ft.cols].index('FileSize')
    first = max([r[seq_col] for r in target.tables['File'].rows] + [0]) + 1
    first = max(first, 10000)
    order = sorted(files)
    for r in ft.rows:
        if r[0] in files:
            r[seq_col] = first + order.index(r[0])
            r[size_col] = len(files[r[0]])
    last = first + len(order) - 1
    pc = {r[0]: r[1] for r in target.tables['Property'].rows}['ProductCode']
    # ":#T": the patch's own rows on top of the upgraded product
    withpatch = up.copy()
    cab = f'PCW_CAB_{name}'
    withpatch.add('Media', (disk, last, '', '#' + cab, '', ''))
    withpatch.add('PatchPackage', (patch_code, disk))
    root = Storage(MSP_CLSID)
    root.items['T1'] = transform(target, up, transform_summary(target, up))
    root.items['#T1'] = transform(up, withpatch, transform_summary(up, withpatch, 0))
    root.items[encode_name(cab)] = cabinet([(k, files[k]) for k in order])
    root.items['\x05SummaryInformation'] = summary_info({
        PID_TITLE: 'Patch', PID_SUBJECT: name, PID_AUTHOR: 'NovaOS', PID_KEYWORDS: 'Installer,Patching,PCP',
        PID_COMMENTS: 'A test patch', PID_TEMPLATE: pc, PID_LASTAUTHOR: ':T1;:#T1', PID_REVNUMBER: patch_code,
        PID_WORDCOUNT: 4, PID_SECURITY: 4,
    })
    open(path, 'wb').write(write_cfb(root))


# -----------------------------------------------------------------------
# A small package
# -----------------------------------------------------------------------
STD_SEQUENCE = [('CostInitialize', 800), ('FileCost', 900), ('CostFinalize', 1000), ('InstallValidate', 1400),
                ('InstallInitialize', 1500), ('ProcessComponents', 1600), ('StopServices', 1900),
                ('DeleteServices', 2000), ('RemoveRegistryValues', 2600), ('RemoveShortcuts', 3200),
                ('RemoveFiles', 3500), ('RemoveFolders', 3600), ('CreateFolders', 3700), ('InstallFiles', 4000),
                ('CreateShortcuts', 4500), ('WriteRegistryValues', 5000), ('WriteEnvironmentStrings', 5200),
                ('InstallServices', 5800), ('StartServices', 5900), ('RegisterProduct', 6100),
                ('InstallFinalize', 6600)]


def package(name, version, product_code, upgrade_code, folder, files, extra_props=()):
    """A package installing @files ([(key, file name, bytes)]) into
    C:\\Programs\\@folder, one component per file, in one feature"""
    db = Database()
    db.summary = {PID_TITLE: 'Installation Database', PID_SUBJECT: name, PID_AUTHOR: 'NovaOS',
                  PID_TEMPLATE: 'x64;1033', PID_REVNUMBER: product_code, PID_PAGECOUNT: 200,
                  PID_WORDCOUNT: 2, PID_SECURITY: 2}
    for k, v in [('ProductName', name), ('ProductVersion', version), ('Manufacturer', 'NovaOS Project'),
                 ('ProductCode', product_code), ('UpgradeCode', upgrade_code), ('ProductLanguage', '1033'),
                 ('ALLUSERS', '1')] + list(extra_props):
        db.add('Property', (k, v))
    db.add('Directory', ('TARGETDIR', None, 'SourceDir'), ('ProgramFiles64Folder', 'TARGETDIR', 'PFiles'),
           ('INSTALLDIR', 'ProgramFiles64Folder', folder))
    db.add('Feature', ('Complete', None, name, None, 1, 1, 'INSTALLDIR', 0))
    files_cab = []
    for seq, (key, fname, data) in enumerate(files, 1):
        comp = 'C_' + key
        db.add('Component', (comp, '{%08X-0000-4000-8000-%012X}' % (zlib.crc32(product_code.encode()), seq),
                             'INSTALLDIR', 256, None, key))
        db.add('FeatureComponents', ('Complete', comp))
        db.add('File', (key, comp, fname, len(data), None, None, 512, seq))
        files_cab.append((key, data))
    db.add('Media', (1, len(files), None, '#data.cab', None, None))
    db.streams['data.cab'] = cabinet(files_cab)
    for a, s in STD_SEQUENCE:
        db.add('InstallExecuteSequence', (a, None, s))
    return db


def now_filetime():
    return int((time.time() + 11644473600) * 10000000)
